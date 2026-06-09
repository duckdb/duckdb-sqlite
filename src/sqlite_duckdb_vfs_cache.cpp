//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_duckdb_vfs_cache.cpp
//
//
//===----------------------------------------------------------------------===//

#include "sqlite_duckdb_vfs_cache.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/re2_regex.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/atomic.hpp"

#include <cstring>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Concurrency Design
//===--------------------------------------------------------------------===//
// One ClientContext drives PARALLEL scans (SqliteMaxThreads may be >1), so several SQLite
// connections - each its own sqlite3_file - share one per-context VFS wrapper at once, and Open(),
// Close() race Unregister() at connection teardown. A per-wrapper open-handle refcount, mutated only
// under registry_mutex, governs wrapper lifetime. Unregister() retires a wrapper rather than freeing
// it: sqlite3_open_v2 resolves the sqlite3_vfs pointer before calling xOpen, so a synchronous free can
// race an in-flight xOpen. A retired wrapper with live handles is reaped by its last Close(); a
// zero-handle one by the next teardown's ReapIdleRetiredWrappers(). A closing handle therefore never
// dereferences freed io_methods.
// SQLite never issues concurrent calls on one sqlite3_file, so per-handle ops (Read/FileSize/...)
// need no locking; the wrapper-shared last_error is mutex-guarded but best-effort (a later failure
// can overwrite an earlier one). Lock order: registry_mutex is the outer lock - the GetLastError /
// ClearLastErrorForContext path takes it and then calls into the wrapper's error_mutex, so never
// acquire registry_mutex while holding error_mutex. ClientContext must outlive all its SQLite
// connections; the shared ExternalFileCache lives at the DatabaseInstance level and is synchronized
// by DuckDB.
//===--------------------------------------------------------------------===//

// Per-context VFS instances. vfs_name uses sqlite3_malloc for cross-DLL safety.
struct DuckDBVFSWrapper {
	sqlite3_vfs base; // Must be first - SQLite VFS structure
	ClientContext *context;
	char *vfs_name;
	sqlite3_io_methods io_methods;

	mutable mutex error_mutex;
	string last_error_message;

	// Open-handle refcount + retire flag; read and written only under registry_mutex.
	idx_t open_file_count = 0;
	bool pending_unregister = false;

	~DuckDBVFSWrapper() noexcept {
		if (vfs_name) {
			sqlite3_free(vfs_name);
			vfs_name = nullptr;
		}
	}

	void SetLastError(const string &error) noexcept {
		// Best-effort: recording a diagnostic must never throw out of a VFS callback into SQLite's C frame.
		try {
			lock_guard<mutex> lock(error_mutex);
			last_error_message = error;
		} catch (...) {
		}
	}

	string GetLastError() const {
		lock_guard<mutex> lock(error_mutex);
		return last_error_message;
	}
};

//===--------------------------------------------------------------------===//
// HTTP Error Mapping
//===--------------------------------------------------------------------===//

// HTTP/network-specific markers only. A bare "exception_type":"IO" is excluded: DuckDB also raises
// IOException for local faults (e.g. a disk-full write to the on-disk block cache), which are not HTTP
// errors. Network failures are identified by the HTTP ExceptionType, the parsed status code, or the
// connection-specific strings below.
static constexpr const char *HTTP_ERROR_PATTERNS[] = {"Unable to connect to URL", "Could not establish connection",
                                                      "HTTP HEAD to", "HTTP GET to"};

static int ExtractHttpStatus(const string &error_msg) {
	// Group 1: "status_code":"XXX" (JSON)
	// Group 2: XXX (Description) (httpfs format)
	// Group 3: (HTTP XXX) or HTTP code XXX or HTTP XXX
	static duckdb_re2::Regex status_regex(
	    "\"status_code\":\"(\\d{3})\"|"         // JSON format
	    "(\\d{3})\\s*\\([^)]+\\)|"              // "404 (Not Found)"
	    "\\(?HTTP\\s+(?:code\\s+)?(\\d{3})\\)?" // "(HTTP 404)", "HTTP code 403", "HTTP 500"
	);

	duckdb_re2::Match match;
	if (duckdb_re2::RegexSearch(error_msg, match, status_regex)) {
		for (idx_t i = 1; i < match.groups.size(); i++) {
			if (!match.groups[i].text.empty()) {
				int32_t result;
				if (TryCast::Operation<string_t, int32_t>(string_t(match.groups[i].text), result)) {
					return result;
				}
			}
		}
	}

	return 0;
}

static bool IsHttpError(const string &error_msg) {
	return std::any_of(std::begin(HTTP_ERROR_PATTERNS), std::end(HTTP_ERROR_PATTERNS),
	                   [&error_msg](const char *pattern) { return StringUtil::Contains(error_msg, pattern); });
}

// Map an HTTP status code to a SQLite error code. Only 404 and 429 get a specific code; every other
// status returns 0 here, so the caller's default applies: SQLITE_CANTOPEN on open, SQLITE_IOERR_READ
// on read.
static int HttpStatusToSqliteError(int http_status) {
	switch (http_status) {
	case 404:
		return SQLITE_CANTOPEN; // SQLite interprets this as "unable to open database file"
	case 429:
		// Too Many Requests. SQLITE_BUSY is the closest "transient, retry" code; SQLite's busy handler
		// does not auto-retry an xRead I/O error, so a mid-scan 429 surfaces to the caller.
		return SQLITE_BUSY;
	default:
		return 0; // Unmapped: the caller's default error code applies.
	}
}

// Extract the HTTP status code from a caught exception. Prefer the structured status_code that
// DuckDB's HTTPException records in its extra-info map; fall back to parsing the message text for
// error sources that do not populate it.
static int GetHttpStatus(const ErrorData &error_data, const string &error_msg) {
	auto &info = error_data.ExtraInfo();
	auto it = info.find("status_code");
	if (it != info.end()) {
		int32_t code;
		if (TryCast::Operation<string_t, int32_t>(string_t(it->second), code)) {
			return code;
		}
	}
	return ExtractHttpStatus(error_msg);
}

// Build a sidecar URL ("-wal" / "-journal") by inserting the suffix before any "?query", so the probe
// targets the right object rather than appending past the query string.
static string BuildSidecarUrl(const string &path, const char *suffix) {
	const auto query_pos = path.find('?');
	return (query_pos == string::npos) ? path + suffix
	                                   : path.substr(0, query_pos) + suffix + path.substr(query_pos);
}

// Classify an exception from a sidecar probe. Returns true only when the sidecar is CONFIRMED absent
// (HTTP 404, or a non-HTTP "not found"): the database has no live sidecar and the main file is safe to
// read. Any other error (403 on a bare presigned URL, a transient 429/503, a timeout) leaves the
// sidecar's state UNKNOWN, so the caller must fail closed rather than assume absent.
static bool SidecarConfirmedAbsent(const std::exception &e) {
	const ErrorData error_data(e);
	const string error_msg = e.what();
	const int http_status = GetHttpStatus(error_data, error_msg);
	return http_status == 404 ||
	       (http_status == 0 && error_data.Type() != ExceptionType::HTTP && !IsHttpError(error_msg) &&
	        (StringUtil::Contains(error_msg, "No files found") ||
	         StringUtil::Contains(error_msg, "does not exist") || StringUtil::Contains(error_msg, "No such file")));
}

template <typename T, typename Func>
static T SafeVFSCall(T error_value, Func &&func, DuckDBVFSWrapper *wrapper = nullptr, const char *path = nullptr) {
	// The error-mapping below compares error_value against SQLITE_OK and returns SQLite error codes,
	// so it is only meaningful for int returns. (xGetLastError, whose return is an OS errno rather
	// than a SQLite code, does not route through here.)
	static_assert(std::is_same<T, int>::value, "SafeVFSCall maps to SQLite error codes; T must be int");
	try {
		return func();
	} catch (const std::exception &e) {
		// SafeVFSCall exists to keep C++ exceptions out of SQLite's C call frame, but the diagnostic
		// and mapping work below itself allocates (string copies, ErrorData JSON parse, regex,
		// SetLastError) and can throw (e.g. std::bad_alloc, under the same memory pressure that
		// produced the original failure). An inner guard degrades a failed diagnosis to the safe
		// default error code rather than letting it escape into C (which would be UB).
		try {
			const string error_msg = e.what();
			const ErrorData error_data(e);
			const int http_status = GetHttpStatus(error_data, error_msg);
			// Clean, human-readable message for the stored error (DuckDB serializes HTTPException as
			// JSON in what(); RawMessage() is just the "exception_message", e.g. the "HTTP GET error
			// on '...' (HTTP 404 Not Found)" text). error_msg keeps the raw form for status parsing.
			const string &clean_msg = error_data.RawMessage();

			if (http_status != 0 || error_data.Type() == ExceptionType::HTTP || IsHttpError(error_msg)) {
				if (wrapper) {
					string full_error = "HTTP Error: ";
					full_error += clean_msg;
					if (path) {
						full_error += " (URL: ";
						full_error += path;
						full_error += ")";
					}
					wrapper->SetLastError(full_error);
				}

				int sqlite_error = HttpStatusToSqliteError(http_status);
				if (sqlite_error != 0) {
					return sqlite_error;
				}

				if (error_msg.find("Unable to connect to URL") != string::npos ||
				    error_msg.find("Could not establish connection") != string::npos) {
					return error_value == SQLITE_OK ? SQLITE_CANTOPEN : error_value;
				}

				return error_value == SQLITE_OK ? SQLITE_IOERR : error_value;
			}

			if (error_data.Type() == ExceptionType::PERMISSION ||
			    error_msg.find("Permission") != string::npos) {
				if (wrapper) {
					string full_error = "Permission denied: ";
					full_error += clean_msg;
					if (path) {
						full_error += " (Path: ";
						full_error += path;
						full_error += ")";
					}
					wrapper->SetLastError(full_error);
				}
				return error_value == SQLITE_OK ? SQLITE_PERM : error_value;
			}

			if (wrapper) {
				string full_error = "Error: ";
				full_error += clean_msg;
				if (path) {
					full_error += " (Path: ";
					full_error += path;
					full_error += ")";
				}
				wrapper->SetLastError(full_error);
			}

			return error_value;
		} catch (...) {
			// The diagnosis/mapping itself threw (e.g. allocation failure) - return the safe default.
			return error_value;
		}
	} catch (...) {
		// Unknown (non-std) exception. SetLastError allocates, so guard it too.
		try {
			if (wrapper) {
				wrapper->SetLastError("Unknown error occurred");
			}
		} catch (...) {
		}
		return error_value;
	}
}

struct VFSRegistryData {
	mutex registry_mutex;
	unordered_map<ClientContext *, unique_ptr<DuckDBVFSWrapper>> registry;
	// Wrappers Unregister() retired; reaped by the last Close() (handles still open) or by
	// ReapIdleRetiredWrappers() at the next teardown (retired with zero handles).
	vector<unique_ptr<DuckDBVFSWrapper>> retired;
};

static VFSRegistryData &GetVFSRegistryData() {
	static VFSRegistryData data;
	return data;
}

// Free zero-handle retired wrappers. Runs under registry_mutex, called only from Register/Unregister.
// A wrapper is retired only by Unregister(context), which DuckDB invokes from OnConnectionClosed once
// that context is quiescent: opens on a context's per-context VFS are issued only by that context, so
// once it is closing none is in flight and none can start. A retired wrapper at open_file_count == 0
// therefore has no live or pending sqlite3_open_v2 resolving its sqlite3_vfs pointer, and freeing it
// here is safe. Wrappers retired with live handles are instead reaped by their last Close().
static void ReapIdleRetiredWrappers(VFSRegistryData &registry_data) {
	auto &retired = registry_data.retired;
	for (auto it = retired.begin(); it != retired.end();) {
		if ((*it)->open_file_count == 0) {
			it = retired.erase(it);
		} else {
			++it;
		}
	}
}

static constexpr int SQLITE_SECTOR_SIZE = 4096;

static void InitializeIOMethods(sqlite3_io_methods &io_methods) {
	memset(&io_methods, 0, sizeof(io_methods));

	io_methods.iVersion = 1;
	io_methods.xClose = SQLiteDuckDBCacheVFS::Close;
	io_methods.xRead = SQLiteDuckDBCacheVFS::Read;
	io_methods.xWrite = SQLiteDuckDBCacheVFS::Write;
	io_methods.xTruncate = SQLiteDuckDBCacheVFS::Truncate;
	io_methods.xSync = SQLiteDuckDBCacheVFS::Sync;
	io_methods.xFileSize = SQLiteDuckDBCacheVFS::FileSize;
	io_methods.xLock = SQLiteDuckDBCacheVFS::Lock;
	io_methods.xUnlock = SQLiteDuckDBCacheVFS::Unlock;
	io_methods.xCheckReservedLock = SQLiteDuckDBCacheVFS::CheckReservedLock;
	io_methods.xFileControl = SQLiteDuckDBCacheVFS::FileControl;
	io_methods.xSectorSize = SQLiteDuckDBCacheVFS::SectorSize;
	io_methods.xDeviceCharacteristics = SQLiteDuckDBCacheVFS::DeviceCharacteristics;
	io_methods.xShmMap = nullptr;
	io_methods.xShmLock = nullptr;
	io_methods.xShmBarrier = nullptr;
	io_methods.xShmUnmap = nullptr;
	io_methods.xFetch = nullptr;
	io_methods.xUnfetch = nullptr;
}

static string GetUniqueVFSName() {
	static atomic<uint64_t> vfs_counter {0};
	return "duckdb_cache_vfs_" + to_string(vfs_counter.fetch_add(1));
}

//===--------------------------------------------------------------------===//
// DuckDBCachedFile Implementation
//===--------------------------------------------------------------------===//

DuckDBCachedFile::DuckDBCachedFile(ClientContext &context, const string &path) : context(context), path(path) {
	// Defer open: DuckDB operations must not run inside the VFS callback frame.
}

void DuckDBCachedFile::EnsureInitialized() {
	if (initialized) {
		return;
	}
	// A prior attempt failed (e.g. WAL rejection, or a network error fetching the header). Reads on a
	// remote file go through SQLite's pager, which retries xRead many times per query; without this
	// short-circuit each retry would re-issue the open's HTTP round-trips and the rich failure reason
	// (e.g. the WAL message) would be lost behind a generic per-read I/O error.
	if (init_failed) {
		throw IOException(init_error_message);
	}
	try {
		InitializeFromRemote();
	} catch (const std::exception &e) {
		init_failed = true;
		init_error_message = ErrorData(e).RawMessage();
		throw;
	}
}

void DuckDBCachedFile::InitializeFromRemote() {
	// DIRECT_IO: bypass OS page cache; DuckDB's CachingFileSystem provides its own block cache.
	auto flags = FileFlags::FILE_FLAGS_READ;
	if (FileSystem::IsRemoteFile(path)) {
		flags |= FileFlags::FILE_FLAGS_DIRECT_IO;
	}

	// Build into locals and commit to members only once every step succeeds, so a throw on a retry
	// leaves the prior members intact. (caching_fs must outlive caching_handle; see the member decl.)
	auto new_fs = make_uniq<CachingFileSystem>(CachingFileSystem::Get(context));
	OpenFileInfo file_info(path);
	// enable_external_access (and the allow-list) is enforced here: CachingFileSystem wraps the
	// context's OpenerFileSystem, whose OpenFile throws PermissionException before any network I/O
	// when the path is not permitted. The VFS adds no bypass; it must open via the context filesystem
	// (no explicit opener) so that gate always applies.
	auto new_handle = new_fs->OpenFile(file_info, flags);
	// Fetch the size once: the remote file is read-only and immutable, so this avoids a per-read size
	// lookup that could otherwise cost an HTTP round trip.
	const auto size = static_cast<sqlite3_int64>(new_handle->GetFileSize());

	// This VFS serves only the main database file and advertises SQLITE_IOCAP_IMMUTABLE, so SQLite skips
	// hot-journal/WAL recovery. A database with a live sidecar - a populated "-wal" (WAL mode) or a hot
	// "-journal" (an interrupted rollback-mode write) - would then be read as if clean, silently serving
	// a stale or inconsistent snapshot. Verify no live sidecar before serving: header byte 19 (the
	// read-format version) picks which sidecar to check - 2 = WAL, 1 = rollback journal, the same byte
	// SQLite's own pager keys WAL-mode access off. Each probe fails CLOSED unless the sidecar is
	// confirmed absent (see SidecarConfirmedAbsent).
	if (size >= 20) {
		data_t header[20];
		new_handle->Read(20, 0).CopyTo(data_ptr_cast(header), 20);
		const bool is_sqlite = memcmp(header, "SQLite format 3", 16) == 0;
		const bool wal_mode = header[19] >= 2;
		if (is_sqlite && wal_mode) {
			// WAL mode: reject a "-wal" that holds more than its 32-byte header (i.e. has frames).
			const string wal_path = BuildSidecarUrl(path, "-wal");
			idx_t wal_size = 0;
			try {
				wal_size = new_fs->OpenFile(OpenFileInfo(wal_path), flags)->GetFileSize();
			} catch (const std::exception &e) {
				if (!SidecarConfirmedAbsent(e)) {
					throw IOException(
					    "Cannot verify whether remote SQLite database \"%s\" is in WAL mode: probing its "
					    "\"-wal\" sidecar failed (%s). Refusing to read rather than risk serving a stale "
					    "snapshot. Checkpoint the database into a single file (PRAGMA "
					    "wal_checkpoint(TRUNCATE), or VACUUM INTO), or make the \"-wal\" URL reachable.",
					    path, ErrorData(e).RawMessage());
				}
				wal_size = 0; // confirmed absent: the main file is checkpointed-complete
			}
			if (wal_size > 32) {
				throw IOException("Cannot read remote SQLite database \"%s\" in WAL mode: its \"-wal\" sidecar "
				                  "holds changes this read-only reader cannot apply. Checkpoint it into a single "
				                  "file (PRAGMA wal_checkpoint(TRUNCATE), or VACUUM INTO) before serving it.",
				                  path);
			}
		} else if (is_sqlite) {
			// Rollback-journal mode: a hot "-journal" holds uncommitted changes a normal opener would roll
			// back, which this read-only reader cannot. Mirror SQLite's hasHotJournal(): a journal with a
			// nonzero first byte is hot. A clean commit removes it (DELETE), empties it (TRUNCATE), or
			// zeroes its header (PERSIST), so a zeroed header passes. Keying on the first byte is
			// conservative: a corrupt-header leftover is refused, but a dirty main file is never served.
			const string journal_path = BuildSidecarUrl(path, "-journal");
			bool hot_journal = false;
			try {
				auto journal_handle = new_fs->OpenFile(OpenFileInfo(journal_path), flags);
				if (journal_handle->GetFileSize() > 0) {
					data_t first = 0;
					journal_handle->Read(1, 0).CopyTo(data_ptr_cast(&first), 1);
					hot_journal = first != 0;
				}
			} catch (const std::exception &e) {
				if (!SidecarConfirmedAbsent(e)) {
					throw IOException(
					    "Cannot verify whether remote SQLite database \"%s\" has a hot rollback journal: "
					    "probing its \"-journal\" sidecar failed (%s). Refusing to read rather than risk "
					    "serving an inconsistent snapshot. Serve a cleanly-closed database, or make the "
					    "\"-journal\" URL reachable.",
					    path, ErrorData(e).RawMessage());
				}
				// confirmed absent: no journal, the main file is clean.
			}
			if (hot_journal) {
				throw IOException("Cannot read remote SQLite database \"%s\": its \"-journal\" sidecar holds a "
				                  "hot rollback journal from an interrupted write, which this read-only reader "
				                  "cannot replay. Serve a cleanly-closed database.",
				                  path);
			}
		}
	}

	caching_fs = std::move(new_fs);
	caching_handle = std::move(new_handle);
	cached_file_size = size;
	initialized = true;
}

int DuckDBCachedFile::Read(void *buffer, int amount, sqlite3_int64 offset) {
	if (offset < 0 || amount < 0) {
		return SQLITE_IOERR_READ;
	}

	if (!buffer || amount == 0) {
		return SQLITE_OK;
	}

	EnsureInitialized();

	if (!caching_handle) {
		return SQLITE_IOERR_READ;
	}

	const sqlite3_int64 file_size = cached_file_size;
	if (file_size < 0) {
		return SQLITE_IOERR_READ;
	}

	if (offset >= file_size) {
		memset(buffer, 0, amount);
		return SQLITE_IOERR_SHORT_READ;
	}

	const sqlite3_int64 available_bytes = file_size - offset;
	const int bytes_to_read = (available_bytes < amount) ? static_cast<int>(available_bytes) : amount;

	auto buffer_group = caching_handle->Read(static_cast<idx_t>(bytes_to_read), offset);
	buffer_group.CopyTo(data_ptr_cast(buffer), static_cast<idx_t>(bytes_to_read));

	if (bytes_to_read < amount) {
		memset(static_cast<char *>(buffer) + bytes_to_read, 0, amount - bytes_to_read);
	}

	return (bytes_to_read < amount) ? SQLITE_IOERR_SHORT_READ : SQLITE_OK;
}

sqlite3_int64 DuckDBCachedFile::GetFileSize() {
	EnsureInitialized();
	return cached_file_size;
}

//===--------------------------------------------------------------------===//
// SQLiteDuckDBCacheVFS Implementation
//===--------------------------------------------------------------------===//

bool SQLiteDuckDBCacheVFS::CanHandlePath(const string &path) {
	// FileSystem::IsRemoteFile matches every remote filesystem DuckDB or its extensions register
	// (http/https/s3/gcs/azure/hf/...). Plain local files on a native build stay on native SQLite for
	// the read-write, locking, and WAL that a read-only VFS cannot provide.
	if (FileSystem::IsRemoteFile(path)) {
		return true;
	}
#if defined(__EMSCRIPTEN__)
	// On WASM there is no native file access, so route local paths through DuckDB's FileSystem too.
	return true;
#else
	return false;
#endif
}

void SQLiteDuckDBCacheVFS::Register(ClientContext &context) {
	auto &registry_data = GetVFSRegistryData();
	lock_guard<mutex> lock(registry_data.registry_mutex);

	// Reclaim zero-handle wrappers retired by an earlier teardown (bounds the retired list under churn).
	ReapIdleRetiredWrappers(registry_data);

	auto it = registry_data.registry.find(&context);
	if (it != registry_data.registry.end()) {
		return;
	}

	sqlite3_vfs *default_vfs = sqlite3_vfs_find(nullptr);
	if (!default_vfs) {
		throw InternalException("Failed to find default SQLite VFS - SQLite may not be properly initialized");
	}

	auto wrapper = make_uniq<DuckDBVFSWrapper>();
	wrapper->context = &context;

	// sqlite3_malloc: ownership must match SQLite's allocator when the name crosses DLL boundaries.
	const string temp_name = GetUniqueVFSName();
	wrapper->vfs_name = static_cast<char *>(sqlite3_malloc64(temp_name.length() + 1));
	if (!wrapper->vfs_name) {
		throw InternalException("Failed to allocate memory for VFS name");
	}
	memcpy(wrapper->vfs_name, temp_name.c_str(), temp_name.length() + 1);

	InitializeIOMethods(wrapper->io_methods);

	memset(&wrapper->base, 0, sizeof(wrapper->base));
	wrapper->base.iVersion = 1;
	wrapper->base.szOsFile = sizeof(SQLiteDuckDBCachedFile);
	// Our pathnames are arbitrary-length URLs. The default OS VFS's mxPathname (512 on unix) is too
	// small for presigned S3/GCS/Azure URLs, which SQLite rejects (its journal-path guard checks
	// nPathname+8 > mxPathname) before xOpen runs.
	wrapper->base.mxPathname = 8192;
	wrapper->base.zName = wrapper->vfs_name;
	wrapper->base.pAppData = wrapper.get();

	wrapper->base.xOpen = Open;
	wrapper->base.xDelete = Delete;
	wrapper->base.xAccess = Access;
	wrapper->base.xFullPathname = FullPathname;
	wrapper->base.xDlOpen = DlOpen;
	wrapper->base.xDlError = DlError;
	wrapper->base.xDlSym = DlSym;
	wrapper->base.xDlClose = DlClose;
	wrapper->base.xRandomness = Randomness;
	wrapper->base.xSleep = Sleep;
	wrapper->base.xCurrentTime = CurrentTime;
	wrapper->base.xGetLastError = GetLastError;

	// Take ownership in the registry before handing the VFS to SQLite, so a throwing map insert leaves
	// nothing registered and SQLite's global VFS list never points at freed wrapper memory.
	auto *wrapper_ptr = wrapper.get();
	registry_data.registry[&context] = std::move(wrapper);

	int rc = sqlite3_vfs_register(&wrapper_ptr->base, 0);
	if (rc != SQLITE_OK) {
		registry_data.registry.erase(&context); // destroys the wrapper; wrapper_ptr is dead past here
		wrapper_ptr = nullptr;                   // guard against any future use after the erase
		throw InternalException("Failed to register DuckDB Cache VFS: %s", sqlite3_errstr(rc));
	}
}

void SQLiteDuckDBCacheVFS::Unregister(ClientContext &context) {
	auto &registry_data = GetVFSRegistryData();
	lock_guard<mutex> lock(registry_data.registry_mutex);

	// Reclaim wrappers retired (with zero handles) by an earlier teardown before retiring this one.
	ReapIdleRetiredWrappers(registry_data);

	auto it = registry_data.registry.find(&context);
	if (it == registry_data.registry.end()) {
		return;
	}
	// Remove the VFS from SQLite's name table so no new connection can open against it. This does
	// not affect connections already open against it - they keep their sqlite3_vfs pointer.
	sqlite3_vfs_unregister(&it->second->base);
	// Retire, never free synchronously: an in-flight xOpen may still hold this wrapper (see the
	// Concurrency Design note). The last Close() or the next ReapIdleRetiredWrappers() reaps it.
	it->second->pending_unregister = true;
	registry_data.retired.push_back(std::move(it->second));
	registry_data.registry.erase(it);
}

string SQLiteDuckDBCacheVFS::GetVFSNameForContext(ClientContext &context) {
	auto &registry_data = GetVFSRegistryData();
	lock_guard<mutex> lock(registry_data.registry_mutex);

	auto it = registry_data.registry.find(&context);
	if (it != registry_data.registry.end() && it->second->vfs_name) {
		// Copy into a std::string while holding the lock, so the returned name stays valid even if
		// another thread unregisters this context's VFS (which frees vfs_name) afterwards.
		return string(it->second->vfs_name);
	}

	// Register() is always called before this for a remote open; reaching here means a logic error
	// (e.g. a caller skipped Register). Fail loudly rather than returning the static default name,
	// which could match a stale VFS from a previously-closed context or no VFS at all.
	throw InternalException("DuckDB cache VFS not registered for this context");
}

string SQLiteDuckDBCacheVFS::GetLastErrorForContext(ClientContext &context) {
	auto &registry_data = GetVFSRegistryData();
	lock_guard<mutex> lock(registry_data.registry_mutex);

	auto it = registry_data.registry.find(&context);
	if (it != registry_data.registry.end()) {
		// Copy the message out while holding the lock (the wrapper, and its mutex-protected
		// string, can be freed by Unregister once the lock is released).
		return it->second->GetLastError();
	}
	return string();
}

void SQLiteDuckDBCacheVFS::ClearLastErrorForContext(ClientContext &context) {
	auto &registry_data = GetVFSRegistryData();
	lock_guard<mutex> lock(registry_data.registry_mutex);

	auto it = registry_data.registry.find(&context);
	if (it != registry_data.registry.end()) {
		// Reset before an open attempt so any error read afterwards belongs to THIS attempt
		// (the recorded message persists across opens on the same context otherwise).
		it->second->SetLastError("");
	}
}

//===--------------------------------------------------------------------===//
// VFS Methods
//===--------------------------------------------------------------------===//

// System-level ops (randomness, sleep, time) have no remote file I/O; delegate to the default VFS.
#define DELEGATE_TO_DEFAULT_VFS(method_name, ...)                                                                      \
	sqlite3_vfs *default_vfs = sqlite3_vfs_find(nullptr);                                                              \
	if (default_vfs && default_vfs->method_name) {                                                                     \
		return default_vfs->method_name(default_vfs, __VA_ARGS__);                                                     \
	}                                                                                                                  \
	return SQLITE_OK;

// Drop one open-handle refcount; if it was the last handle on a retired wrapper, reap it. Acquires
// registry_mutex internally (so the decrement and the reap decision cannot interleave with
// Unregister()'s count check); callers must not already hold it.
static void ReleaseWrapperHandle(DuckDBVFSWrapper *wrapper) {
	if (!wrapper) {
		return;
	}
	auto &registry_data = GetVFSRegistryData();
	lock_guard<mutex> lock(registry_data.registry_mutex);
	// Each live handle holds one count (reserved in Open() before io_methods is published). A decrement
	// at zero is a double-release; catch it before the unsigned wrap turns into an un-reapable leak.
	D_ASSERT(wrapper->open_file_count > 0);
	if (--wrapper->open_file_count == 0 &&
	    wrapper->pending_unregister) {
		for (auto it = registry_data.retired.begin(); it != registry_data.retired.end(); ++it) {
			if (it->get() == wrapper) {
				registry_data.retired.erase(it);
				break;
			}
		}
	}
}

int SQLiteDuckDBCacheVFS::Open(sqlite3_vfs *vfs, const char *filename, sqlite3_file *file, int flags, int *out_flags) {
	auto *wrapper = vfs && vfs->pAppData ? static_cast<DuckDBVFSWrapper *>(vfs->pAppData) : nullptr;

	return SafeVFSCall<int>(
	    SQLITE_CANTOPEN,
	    [&]() {
		    if (!vfs || !filename || !file || (flags & SQLITE_OPEN_READONLY) == 0) {
			    return SQLITE_CANTOPEN;
		    }

		    // SQLite's VFS contract requires xOpen to leave file->pMethods == nullptr on every failure
		    // path, so xClose is never invoked on a half-open handle. Set it explicitly rather than
		    // relying on SQLite to pre-zero the struct; the success path overwrites it below.
		    file->pMethods = nullptr;

		    if (vfs->szOsFile < static_cast<int>(sizeof(SQLiteDuckDBCachedFile))) {
			    return SQLITE_CANTOPEN;
		    }

		    auto *duckdb_file = reinterpret_cast<SQLiteDuckDBCachedFile *>(file);

		    if (!vfs->pAppData) {
			    return SQLITE_CANTOPEN;
		    }

		    ClientContext *context = wrapper->context;

		    if (!context || !context->db) {
			    return SQLITE_CANTOPEN;
		    }

		    // Reserve the refcount BEFORE publishing io_methods into the sqlite3_file, so a racing
		    // Unregister() observes the handle and retires the wrapper instead of freeing it under us.
		    // Released by Close() (or on the construction-failure path below).
		    {
			    auto &registry_data = GetVFSRegistryData();
			    lock_guard<mutex> lock(registry_data.registry_mutex);
			    wrapper->open_file_count++;
		    }

		    duckdb_file->base.pMethods = &wrapper->io_methods;
		    duckdb_file->duckdb_file = nullptr;
		    duckdb_file->wrapper = wrapper;

		    try {
			    duckdb_file->duckdb_file = new DuckDBCachedFile(*context, filename);
		    } catch (...) {
			    duckdb_file->base.pMethods = nullptr;
			    duckdb_file->duckdb_file = nullptr;
			    duckdb_file->wrapper = nullptr;
			    ReleaseWrapperHandle(wrapper);
			    return SQLITE_CANTOPEN;
		    }

		    // Defer SQLite-header validation to first read: no DuckDB I/O inside the open callback.

		    if (out_flags) {
			    *out_flags = flags;
		    }

		    return SQLITE_OK;
	    },
	    wrapper, filename);
}

int SQLiteDuckDBCacheVFS::Delete(sqlite3_vfs *vfs, const char *filename, int sync_dir) {
	return SQLITE_IOERR_DELETE;
}

int SQLiteDuckDBCacheVFS::Access(sqlite3_vfs *vfs, const char *filename, int flags, int *result) {
	auto *wrapper = vfs && vfs->pAppData ? static_cast<DuckDBVFSWrapper *>(vfs->pAppData) : nullptr;

	return SafeVFSCall<int>(
	    SQLITE_IOERR,
	    [&]() {
		    if (!filename || !result) {
			    return SQLITE_IOERR;
		    }

		    // This VFS serves read-only, immutable databases: DeviceCharacteristics() returns
		    // SQLITE_IOCAP_IMMUTABLE, so SQLite treats the main file as fresh and skips hot-journal/WAL
		    // recovery. We already verified at open time that no live -wal/-journal sidecar is present. Report "does not
		    // exist" for every probe instead of issuing a network round-trip to check.
		    *result = 0;

		    return SQLITE_OK;
	    },
	    wrapper, filename);
}

int SQLiteDuckDBCacheVFS::FullPathname(sqlite3_vfs *vfs, const char *filename, int out_size, char *out_buf) {
	return SafeVFSCall<int>(SQLITE_IOERR, [&]() {
		if (!filename || !out_buf || out_size <= 0) {
			return SQLITE_IOERR;
		}

		// Remote paths are already absolute URLs - return as-is. If the URL does not fit SQLite's
		// buffer (sized from mxPathname), fail clearly: silently truncating would issue the HTTP
		// request against a corrupted URL.
		if (static_cast<int>(strlen(filename)) >= out_size) {
			return SQLITE_CANTOPEN;
		}
		strncpy(out_buf, filename, out_size - 1);
		out_buf[out_size - 1] = '\0';
		return SQLITE_OK;
	});
}

int SQLiteDuckDBCacheVFS::Randomness(sqlite3_vfs *vfs, int bytes, char *out) {
	DELEGATE_TO_DEFAULT_VFS(xRandomness, bytes, out);
}

int SQLiteDuckDBCacheVFS::Sleep(sqlite3_vfs *vfs, int microseconds) {
	DELEGATE_TO_DEFAULT_VFS(xSleep, microseconds);
}

int SQLiteDuckDBCacheVFS::CurrentTime(sqlite3_vfs *vfs, double *time) {
	DELEGATE_TO_DEFAULT_VFS(xCurrentTime, time);
}

void *SQLiteDuckDBCacheVFS::DlOpen(sqlite3_vfs *vfs, const char *filename) {
	return nullptr;
}

void SQLiteDuckDBCacheVFS::DlError(sqlite3_vfs *vfs, int bytes, char *err_msg) {
	try {
		if (err_msg && bytes > 0) {
			strncpy(err_msg, "Dynamic loading not supported through the DuckDB filesystem VFS", bytes - 1);
			err_msg[bytes - 1] = '\0';
		}
	} catch (...) {
		// Best effort - if we can't even set the error message, just return
		if (err_msg && bytes > 0) {
			err_msg[0] = '\0';
		}
	}
}

void (*SQLiteDuckDBCacheVFS::DlSym(sqlite3_vfs *vfs, void *handle, const char *symbol))(void) {
	return nullptr;
}

void SQLiteDuckDBCacheVFS::DlClose(sqlite3_vfs *vfs, void *handle) {
}

int SQLiteDuckDBCacheVFS::GetLastError(sqlite3_vfs *vfs, int bytes, char *err_msg) {
	return SafeVFSCall<int>(0, [&]() {
		if (!vfs || !vfs->pAppData || !err_msg || bytes <= 0) {
			return 0;
		}

		auto *wrapper = static_cast<DuckDBVFSWrapper *>(vfs->pAppData);
		const string error = wrapper->GetLastError();

		if (error.empty()) {
			err_msg[0] = '\0';
			return 0;
		}

		strncpy(err_msg, error.c_str(), bytes - 1);
		err_msg[bytes - 1] = '\0';

		// SQLite uses xGetLastError's return value as an OS error code; we have none, so return 0.
		// The rich error reaches callers through GetLastErrorForContext, not this hook.
		return 0;
	});
}

//===--------------------------------------------------------------------===//
// File Methods
//===--------------------------------------------------------------------===//

int SQLiteDuckDBCacheVFS::Close(sqlite3_file *file) {
	return SafeVFSCall<int>(SQLITE_OK, [&]() {
		if (file) {
			auto *duckdb_file = reinterpret_cast<SQLiteDuckDBCachedFile *>(file);
			delete duckdb_file->duckdb_file;
			duckdb_file->duckdb_file = nullptr;

			// Release our handle; if this was the last one on a retired wrapper, ReleaseWrapperHandle
			// reaps it here - the wrapper was kept alive precisely so this Close() stays valid.
			auto *wrapper = duckdb_file->wrapper;
			duckdb_file->wrapper = nullptr;
			ReleaseWrapperHandle(wrapper);
		}
		return SQLITE_OK;
	});
}

int SQLiteDuckDBCacheVFS::Read(sqlite3_file *file, void *buffer, int amount, sqlite3_int64 offset) {
	// Record the rich error (httpfs status/URL) on the wrapper so HandleOpenError can surface it;
	// the per-file wrapper is set in Open().
	auto *read_file = file ? reinterpret_cast<SQLiteDuckDBCachedFile *>(file) : nullptr;
	auto *wrapper = read_file ? read_file->wrapper : nullptr;
	return SafeVFSCall<int>(
	    SQLITE_IOERR_READ,
	    [&]() {
		    if (!file || !buffer) {
			    return SQLITE_IOERR_READ;
		    }

		    auto *duckdb_file = reinterpret_cast<SQLiteDuckDBCachedFile *>(file);
		    if (!duckdb_file->duckdb_file) {
			    return SQLITE_IOERR_READ;
		    }

		    return duckdb_file->duckdb_file->Read(buffer, amount, offset);
	    },
	    wrapper);
}

int SQLiteDuckDBCacheVFS::FileSize(sqlite3_file *file, sqlite3_int64 *size) {
	auto *size_file = file ? reinterpret_cast<SQLiteDuckDBCachedFile *>(file) : nullptr;
	auto *wrapper = size_file ? size_file->wrapper : nullptr;
	return SafeVFSCall<int>(
	    SQLITE_IOERR,
	    [&]() {
		    if (!file || !size) {
			    return SQLITE_IOERR;
		    }

		    auto *duckdb_file = reinterpret_cast<SQLiteDuckDBCachedFile *>(file);
		    if (!duckdb_file->duckdb_file) {
			    return SQLITE_IOERR;
		    }

		    const sqlite3_int64 file_size = duckdb_file->duckdb_file->GetFileSize();
		    if (file_size < 0) {
			    // -1 means the underlying open/size lookup failed (e.g. a network error). Surface an
			    // I/O error rather than reporting a bogus size and letting SQLite read a phantom file.
			    return SQLITE_IOERR;
		    }
		    *size = file_size;
		    return SQLITE_OK;
	    },
	    wrapper);
}

int SQLiteDuckDBCacheVFS::Write(sqlite3_file *file, const void *buffer, int amount, sqlite3_int64 offset) {
	return SQLITE_READONLY;
}

int SQLiteDuckDBCacheVFS::Truncate(sqlite3_file *file, sqlite3_int64 size) {
	return SQLITE_READONLY;
}

int SQLiteDuckDBCacheVFS::Sync(sqlite3_file *file, int flags) {
	return SQLITE_OK;
}

int SQLiteDuckDBCacheVFS::Lock(sqlite3_file *file, int level) {
	return SQLITE_OK;
}

int SQLiteDuckDBCacheVFS::Unlock(sqlite3_file *file, int level) {
	return SQLITE_OK;
}

int SQLiteDuckDBCacheVFS::CheckReservedLock(sqlite3_file *file, int *result) {
	if (result) {
		*result = 0;
	}
	return SQLITE_OK;
}

int SQLiteDuckDBCacheVFS::FileControl(sqlite3_file *file, int op, void *arg) {
	return SQLITE_NOTFOUND;
}

int SQLiteDuckDBCacheVFS::SectorSize(sqlite3_file *file) {
	return SQLITE_SECTOR_SIZE;
}

int SQLiteDuckDBCacheVFS::DeviceCharacteristics(sqlite3_file *file) {
	// IMMUTABLE tells SQLite the file never changes for the life of the connection, so it skips
	// locking and hot-journal/WAL recovery. This is an axiom the caller must honor, not a property
	// this VFS verifies: if the remote object is mutated mid-query (or a CDN / eventually-consistent
	// store serves a stale block), SQLite will not detect it and may return wrong results. Remote
	// SQLite databases are expected to be served as static, checkpointed snapshots.
	return SQLITE_IOCAP_IMMUTABLE;
}

} // namespace duckdb

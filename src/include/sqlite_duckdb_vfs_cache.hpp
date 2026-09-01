//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_duckdb_vfs_cache.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system.hpp"

#include "sqlite3.h"

namespace duckdb {

class ClientContext;
struct DuckDBVFSWrapper;

class DuckDBCachedFile {
public:
	DuckDBCachedFile(ClientContext &context, const string &path);
	~DuckDBCachedFile() = default;

	int Read(void *buffer, int amount, sqlite3_int64 offset);
	sqlite3_int64 GetFileSize();

private:
	void EnsureInitialized();
	// Open the remote file, fetch its size, and run the WAL-mode guard. Throws on any failure;
	// EnsureInitialized() wraps it to memoize that failure.
	void InitializeFromRemote();

	ClientContext &context;
	const string path;
	// CachingFileHandle stores a *reference* to the CachingFileSystem that created it
	// (and dereferences it on every Read to reach the DatabaseInstance / TaskScheduler),
	// so the CachingFileSystem must outlive the handle. CachingFileSystem::Get() returns
	// a value, so we own it here. Declared before caching_handle so it is destroyed after it.
	unique_ptr<CachingFileSystem> caching_fs;
	unique_ptr<CachingFileHandle> caching_handle;
	bool initialized = false;
	// EnsureInitialized() opens the remote file (an HTTP round trip or two). If that fails it is
	// remembered here so later Read()/GetFileSize() calls fail immediately with the same message
	// rather than re-issuing the network I/O on every SQLite page request.
	bool init_failed = false;
	string init_error_message;
	// File size, fetched once at initialization. The remote file is read-only and immutable,
	// so the size never changes; -1 means "not yet initialized / size lookup failed".
	sqlite3_int64 cached_file_size = -1;
};

class SQLiteDuckDBCacheVFS {
public:
	static void Register(ClientContext &context);
	static void Unregister(ClientContext &context);
	// Returns true when the path should be opened through this VFS (DuckDB's FileSystem) rather than
	// native SQLite: any path DuckDB treats as remote, plus all paths on WASM (no native file I/O).
	static bool CanHandlePath(const string &path);
	// Returned by value so the name stays valid after the registry lock is released
	// (the underlying buffer can be freed by Unregister).
	static string GetVFSNameForContext(ClientContext &context);
	// Returns the rich httpfs error (e.g. "HTTP Error: ... (HTTP NNN)") recorded by this context's VFS.
	// Empty if none. Copied under the registry lock. Enriches the user-facing open error that SQLite
	// otherwise collapses to a terse per-code string.
	static string GetLastErrorForContext(ClientContext &context);
	// Reset the recorded error before an open attempt so a later GetLastErrorForContext reflects only
	// that attempt (the message persists across opens on the same context otherwise).
	static void ClearLastErrorForContext(ClientContext &context);

	// Must be public for C callback registration.
	static int Open(sqlite3_vfs *vfs, const char *filename, sqlite3_file *file, int flags, int *out_flags);
	static int Delete(sqlite3_vfs *vfs, const char *filename, int sync_dir);
	static int Access(sqlite3_vfs *vfs, const char *filename, int flags, int *result);
	static int FullPathname(sqlite3_vfs *vfs, const char *filename, int out_size, char *out_buf);
	static void *DlOpen(sqlite3_vfs *vfs, const char *filename);
	static void DlError(sqlite3_vfs *vfs, int bytes, char *err_msg);
	static void (*DlSym(sqlite3_vfs *vfs, void *handle, const char *symbol))(void);
	static void DlClose(sqlite3_vfs *vfs, void *handle);
	static int Randomness(sqlite3_vfs *vfs, int bytes, char *out);
	static int Sleep(sqlite3_vfs *vfs, int microseconds);
	static int CurrentTime(sqlite3_vfs *vfs, double *time);
	static int GetLastError(sqlite3_vfs *vfs, int bytes, char *err_msg);

	static int Close(sqlite3_file *file);
	static int Read(sqlite3_file *file, void *buffer, int amount, sqlite3_int64 offset);
	static int Write(sqlite3_file *file, const void *buffer, int amount, sqlite3_int64 offset);
	static int Truncate(sqlite3_file *file, sqlite3_int64 size);
	static int Sync(sqlite3_file *file, int flags);
	static int FileSize(sqlite3_file *file, sqlite3_int64 *size);
	static int Lock(sqlite3_file *file, int level);
	static int Unlock(sqlite3_file *file, int level);
	static int CheckReservedLock(sqlite3_file *file, int *result);
	static int FileControl(sqlite3_file *file, int op, void *arg);
	static int SectorSize(sqlite3_file *file);
	static int DeviceCharacteristics(sqlite3_file *file);
};

// Allocated by SQLite; may cross module boundaries -- raw pointers with explicit ownership.
struct SQLiteDuckDBCachedFile {
	sqlite3_file base;                       // Must be first member for C compatibility
	DuckDBCachedFile *duckdb_file = nullptr; // deleted in Close()
	DuckDBVFSWrapper *wrapper = nullptr;     // VFS instance owning the io_methods this file points into;
	                                         // refcounted so the wrapper outlives its open files
};

} // namespace duckdb

#include "duckdb.hpp"

#include "sqlite3.h"
#include "sqlite_db.hpp"
#include "sqlite_utils.hpp"
#include "sqlite_storage.hpp"
#include "storage/sqlite_catalog.hpp"
#include "storage/sqlite_transaction_manager.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {

static string CopyDBFile(FileSystem &local_fs, unique_ptr<FileHandle> fh, const string &sqlite_dir, const string &file_path, const string &attached_name) {
	auto filename = local_fs.ExtractName(file_path);
	if (filename.empty()) {
		filename = attached_name + ".sqlite";
	}
	auto dst_file_path = local_fs.JoinPath(sqlite_dir, filename);
	auto dst = local_fs.OpenFile(dst_file_path,
													FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
	if (!dst) {
			throw IOException("failed to open target file for writing: %s", dst_file_path);
	}
	std::vector<char> buffer;
	buffer.resize(4 * 1024 * 1024); // 4MB large buffer
	while (true) {
			int64_t read = fh->Read(buffer.data(), buffer.size()); // returns bytes read, 0 on EOF
			if (read <= 0) {
				break;
			}
			dst->Write(buffer.data(), read);
	}
	dst->Sync();
	return dst_file_path;
}

static SQLiteDBLocation GetDBLocation(AttachedDatabase &db, ClientContext &context, const string &path, const string &attached_name, const SQLiteOpenOptions &options) {
	if (options.access_mode != AccessMode::READ_ONLY) {
		return SQLiteDBLocation(path);
	}
	auto &fs = FileSystem::Get(db);
	auto fh = fs.OpenFile(path, FileOpenFlags::FILE_FLAGS_READ);
	if (!fh || fh->OnDiskFile()) {
		return SQLiteDBLocation(path);
	}
	auto &db_instance = DatabaseInstance::GetDatabase(context);
	auto &local_fs = FileSystem::GetLocal(db_instance);
	string sqlite_dir;
	try {
		auto sys_tmp_dir = SQLiteUtils::GetSystemTempDirectory(local_fs);
		auto dirname = SQLiteUtils::GenerateRandomDirName("duckdb_sqlite");
		sqlite_dir = local_fs.JoinPath(sys_tmp_dir, dirname);
		local_fs.CreateDirectory(sqlite_dir);
		auto dst_file_path = CopyDBFile(local_fs, std::move(fh), sqlite_dir, path, attached_name);
		return SQLiteDBLocation(sqlite_dir, dst_file_path);
	} catch(const IOException &ex) {
		if (!sqlite_dir.empty()) {
			local_fs.RemoveDirectory(sqlite_dir);
		}
		ErrorData error_data(ex);
		throw IOException("Unable to make a local copy of a remote file \"%s\": %s", path, error_data.RawMessage());
	}
}

static unique_ptr<Catalog> SQLiteAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                        AttachedDatabase &db, const string &name, AttachInfo &info,
                                        AttachOptions &attach_options) {
	SQLiteOpenOptions options;
	options.access_mode = attach_options.access_mode;
	for (auto &entry : attach_options.options) {
		if (StringUtil::CIEquals(entry.first, "busy_timeout")) {
			options.busy_timeout = entry.second.GetValue<uint64_t>();
		} else if (StringUtil::CIEquals(entry.first, "journal_mode")) {
			options.journal_mode = entry.second.ToString();
		} else {
			throw NotImplementedException("Unsupported parameter for SQLite Attach: %s", entry.first);
		}
	}
	auto location = GetDBLocation(db, context, info.path, name, options);
	return make_uniq<SQLiteCatalog>(db, std::move(location), std::move(options));
}

static unique_ptr<TransactionManager> SQLiteCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                     AttachedDatabase &db, Catalog &catalog) {
	auto &sqlite_catalog = catalog.Cast<SQLiteCatalog>();
	return make_uniq<SQLiteTransactionManager>(db, sqlite_catalog);
}

SQLiteStorageExtension::SQLiteStorageExtension() {
	attach = SQLiteAttach;
	create_transaction_manager = SQLiteCreateTransactionManager;
}

} // namespace duckdb

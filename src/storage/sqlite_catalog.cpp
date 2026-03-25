#include "storage/sqlite_catalog.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/storage/database_size.hpp"
#include "sqlite_db.hpp"
#include "storage/sqlite_schema_entry.hpp"
#include "storage/sqlite_transaction.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"

namespace duckdb {

SQLiteCatalog::SQLiteCatalog(AttachedDatabase &db_p, const string &path, SQLiteOpenOptions options_p)
    : Catalog(db_p), path(path), options(std::move(options_p)), in_memory(path == ":memory:"), active_in_memory(false) {
	if (InMemory()) {
		in_memory_db = SQLiteDB::Open(path, options, true);
	}
}

SQLiteCatalog::~SQLiteCatalog() {
}

void SQLiteCatalog::Initialize(bool load_builtin) {
	CreateSchemaInfo info;
	main_schema = make_uniq<SQLiteSchemaEntry>(*this, info);
}

optional_ptr<CatalogEntry> SQLiteCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	throw BinderException("SQLite databases do not support creating new schemas");
}

void SQLiteCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	callback(*main_schema);
}

optional_ptr<SchemaCatalogEntry> SQLiteCatalog::LookupSchema(CatalogTransaction transaction,
                                                             const EntryLookupInfo &schema_lookup,
                                                             OnEntryNotFound if_not_found) {
	auto &schema_name = schema_lookup.GetEntryName();
	if (schema_name == DEFAULT_SCHEMA || schema_name == INVALID_SCHEMA) {
		return main_schema.get();
	}
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return nullptr;
	}
	throw BinderException("SQLite databases only have a single schema - \"%s\"", std::string(DEFAULT_SCHEMA));
}

bool SQLiteCatalog::InMemory() {
	return in_memory;
}

string SQLiteCatalog::GetDBPath() {
	return path;
}

SQLiteDB *SQLiteCatalog::GetInMemoryDatabase() {
	if (!InMemory()) {
		throw InternalException("GetInMemoryDatabase() called on a non-in-memory database");
	}
	lock_guard<mutex> l(in_memory_lock);
	if (active_in_memory) {
		throw TransactionException("Only a single transaction can be active on an "
		                           "in-memory SQLite database at a time");
	}
	active_in_memory = true;
	return &in_memory_db;
}

void SQLiteCatalog::ReleaseInMemoryDatabase() {
	if (!InMemory()) {
		return;
	}
	lock_guard<mutex> l(in_memory_lock);
	if (!active_in_memory) {
		throw InternalException("ReleaseInMemoryDatabase called but there is no "
		                        "active transaction on an in-memory database");
	}
	active_in_memory = false;
}

void SQLiteCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	throw BinderException("SQLite databases do not support dropping schemas");
}

DatabaseSize SQLiteCatalog::GetDatabaseSize(ClientContext &context) {
	DatabaseSize result;

	auto &transaction = SQLiteTransaction::Get(context, *this);
	auto &db = transaction.GetDB();
	result.total_blocks = db.RunPragma("page_count");
	result.block_size = db.RunPragma("page_size");
	result.free_blocks = db.RunPragma("freelist_count");
	result.used_blocks = result.total_blocks - result.free_blocks;
	result.bytes = result.total_blocks * result.block_size;
	result.wal_size = idx_t(-1);
	return result;
}

} // namespace duckdb

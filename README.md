# DuckDB SQLite extension

The SQLite extension allows DuckDB to directly read and write data from a SQLite database file. The data can be queried directly from the underlying SQLite tables. Data can be loaded from SQLite tables into DuckDB tables, or vice versa.

## Reading Data from SQLite

To make a SQLite file accessible to DuckDB, use the `ATTACH` command, for example with the bundled `sakila.db` file:

```sql
ATTACH 'data/db/sakila.db' AS sakila;
USE sakila;
```

The tables in the file can be read as if they were normal DuckDB tables, but the underlying data is read directly from the SQLite tables in the file at query time.

```sql
SHOW TABLES;
```

You can query the tables using SQL, e.g. using the example queries from sakila-examples.sql

```sql
SELECT cat.name category_name,
       Sum(Ifnull(pay.amount, 0)) revenue
FROM   category cat
       LEFT JOIN film_category flm_cat
              ON cat.category_id = flm_cat.category_id
       LEFT JOIN film fil
              ON flm_cat.film_id = fil.film_id
       LEFT JOIN inventory inv
              ON fil.film_id = inv.film_id
       LEFT JOIN rental ren
              ON inv.inventory_id = ren.inventory_id
       LEFT JOIN payment pay
              ON ren.rental_id = pay.rental_id
GROUP  BY cat.name
ORDER  BY revenue DESC
LIMIT  5;
```

## Opening SQLite Databases Directly

SQLite databases can also be opened directly and can be used transparently instead of a DuckDB database file. In any client, when connecting, a path to a SQLite database file can be provided and the SQLite database will be opened instead.

For example, with the shell:

```sql
$ > duckdb data/db/sakila.db
v0.9.1 401c8061c6
D SHOW tables;
┌────────────────────────┐
│          name          │
│        varchar         │
├────────────────────────┤
│ actor                  │
│ address                │
│ category               │
│ city                   │
│ country                │
│ customer               │
│ customer_list          │
│ film                   │
│ film_actor             │
│ film_category          │
│ film_list              │
│ film_text              │
│ inventory              │
│ language               │
│ payment                │
│ rental                 │
│ sales_by_film_category │
│ sales_by_store         │
│ staff                  │
│ staff_list             │
│ store                  │
├────────────────────────┤
│        21 rows         │
└────────────────────────┘
```

## Writing Data to SQLite

In addition to reading data from SQLite, the extension also allows you to create new SQLite database files, create tables, ingest data into SQLite and make other modifications to SQLite database files using standard SQL queries.

This allows you to use DuckDB to, for example, export data that is stored in a SQLite database to Parquet, or read data from a Parquet file into SQLite.

Below is a brief example of how to create a new SQLite database and load data into it.

```sql
ATTACH 'new_sqlite_database.db' AS sqlite_db (TYPE SQLITE);
CREATE TABLE sqlite_db.tbl(id INTEGER, name VARCHAR);
INSERT INTO sqlite_db.tbl VALUES (42, 'DuckDB');
```

The resulting SQLite database can then be read into from SQLite.

```sql
$r > sqlite3 new_sqlite_database.db
SQLite version 3.39.5 2022-10-14 20:58:05
sqlite> SELECT * FROM tbl;
id  name
--  ------
42  DuckDB
```

Many operations on SQLite tables are supported. All these operations directly modify the SQLite database, and the result of subsequent operations can then be read using SQLite.

Below is a list of supported operations.

###### CREATE TABLE
```sql
CREATE TABLE sqlite_db.tbl(id INTEGER, name VARCHAR);
```

###### INSERT INTO
```sql
INSERT INTO sqlite_db.tbl VALUES (42, 'DuckDB');
```

###### SELECT
```sql
SELECT * FROM sqlite_db.tbl;
┌───────┬─────────┐
│  id   │  name   │
│ int64 │ varchar │
├───────┼─────────┤
│    42 │ DuckDB  │
└───────┴─────────┘
```

###### COPY
```sql
COPY sqlite_db.tbl TO 'data.parquet';
COPY sqlite_db.tbl FROM 'data.parquet';
```

###### UPDATE
```sql
UPDATE sqlite_db.tbl SET name='Woohoo' WHERE id=42;
```

###### DELETE
```sql
DELETE FROM sqlite_db.tbl WHERE id=42;
```

###### ALTER TABLE
```sql
ALTER TABLE sqlite_db.tbl ADD COLUMN k INTEGER;
```

###### DROP TABLE
```sql
DROP TABLE sqlite_db.tbl;
```

###### CREATE VIEW
```sql
CREATE VIEW sqlite_db.v1 AS SELECT 42;
```

###### Transactions
```sql
CREATE TABLE sqlite_db.tmp(i INTEGER);
BEGIN;
INSERT INTO sqlite_db.tmp VALUES (42);
SELECT * FROM sqlite_db.tmp;
┌───────┐
│   i   │
│ int64 │
├───────┤
│    42 │
└───────┘
ROLLBACK;
SELECT * FROM sqlite_db.tmp;
┌────────┐
│   i    │
│ int64  │
├────────┤
│ 0 rows │
└────────┘
```

## Running Queries in SQLite

The `sqlite_query` table function runs a query directly in an attached SQLite database:

```sql
SELECT * FROM sqlite_query('sqlite_db', 'SELECT name, count(*) FROM tbl GROUP BY name');
```

Statements without a result (e.g. DDL) return the number of changed rows. The function accepts the following named parameters:

* `params` - a `STRUCT` of values for the `?` parameters of the query, e.g. `params = row(42, 'DuckDB')`
* `column_types` - the types of the result columns, e.g. `column_types = ['BIGINT', 'DATE']`. The values are converted to these types whatever SQLite type they are stored with (integers and floats are read as unix timestamps for `DATE` / `TIMESTAMP`, like the table scan reads them). Without it, all result columns are `VARCHAR`. With `column_types`, the query is not prepared before it is executed, so the result columns are named `c0`, `c1`, ... (unless `column_names` are given), and errors in the query (or a wrong number of `column_types`) are only reported when it is executed.
* `column_names` - the names of the result columns when `column_types` are given, e.g. `column_names = ['id', 'day']`
* `preliminary_queries` - statements that are executed (in the same transaction) before the query
* `return_changed_rows` / `suppress_output` - report the number of changed rows like a DuckDB DML statement does / report no result, like a DDL statement
* `clear_catalog_cache` - remove the cached catalog entries of the transaction after executing the statement (e.g. after DDL)

`EXPLAIN` shows the SQL of the query.

## Remote Pushdown

With remote pushdown, queries and statements that only reference tables of a single attached SQLite database are executed in SQLite as a whole (instead of scanning the tables and executing the query in DuckDB), when every construct they use can be translated to SQLite SQL with the same semantics. This covers e.g. joins, aggregates, window functions, CTEs, set operations, subqueries, many scalar functions, `UPDATE` / `DELETE` / `INSERT` (including `RETURNING` and `ON CONFLICT`, which are only supported for SQLite tables with remote pushdown), `CREATE TABLE ... AS`, `CREATE VIEW` and `DROP` / `ALTER TABLE` statements.

Remote pushdown is disabled by default. It can be enabled for newly attached databases (the setting is read when the database is attached):

```sql
SET sqlite_enable_remote_pushdown = true;
ATTACH 'sakila.db' AS sakila (TYPE SQLITE);
```

`EXPLAIN` shows the generated SQL (`Sqlite Query`), and `SET sqlite_debug_show_queries = true` also prints why a query is not pushed down.

```sql
D EXPLAIN SELECT rating, count(*), avg(length) FROM sakila.film WHERE title LIKE 'A%' GROUP BY rating ORDER BY rating;
╭─ Sqlite Query ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╮
│ SQL:                                                                                                                                                                 │
│ SELECT "rating", count(*) AS "count_star()", avg("length") AS "avg(length)" FROM "film" WHERE ("title" GLOB 'A*') GROUP BY "rating" ORDER BY "rating" ASC NULLS LAST │
╰──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
```

Queries that are plain scans of a single table are executed through the regular table scan, which reads the table in parallel (by ranges of the `rowid`). A query is a plain scan when it reads a single table, selects only columns (or `*`), filters only with comparisons of columns with constants, `IN` lists and `IS [NOT] NULL` combined with `AND` / `OR`, and has no `ORDER BY` or `OFFSET` (a `LIMIT` is fine).

The translation takes the differences between DuckDB and SQLite into account, for example:

* Columns are read the way the table scan reads them: `DATE` / `TIMESTAMP` columns can contain ISO strings and unix timestamps, untyped columns are read as `BLOB`, columns with a collation (e.g. `COLLATE NOCASE`) are compared byte-wise like DuckDB compares strings, and with `sqlite_all_varchar` all columns are read as `VARCHAR`.
* SQLite's `LIKE` is case insensitive - `LIKE` with a constant pattern is translated to `GLOB`.
* `a / b` is a floating point division, `NULLS LAST` is the default order, a scalar subquery that returns more than one row raises an error, ...

Where SQLite has no built-in function with the semantics of the DuckDB function, the query uses `duckdb_*` functions that the extension registers on the SQLite connections of databases attached with remote pushdown (e.g. for casts, Unicode `lower` / `upper`, `ILIKE`, regular expressions, date parts, `date_trunc`, `date_diff`, interval arithmetic, `round`, math functions, `stddev`, `median`, `arg_max`). These functions can only be used by top-level SQL (not by the views or triggers stored in a database file), and they are not available to other SQLite clients - views (`CREATE VIEW`) are only stored with translated SQL when it does not use them, and when DuckDB reads the translated SQL back to the same result types. Otherwise a view is stored like without remote pushdown.

A pushed down query is executed by SQLite, single-threaded. This is faster when SQLite can use indexes (e.g. for selective filters, joins and ordering on indexed columns) or when the query reduces the data a lot, but SQLite's query planner and execution engine are not built for analytical queries over large tables - those can be slower than executing the query in DuckDB, which is why remote pushdown is disabled by default. SQLite evaluates correlated subqueries for every row: they are only pushed down when the correlated rows can be looked up through an index (the row id, an `INTEGER PRIMARY KEY` or the leading column of an index), DuckDB decorrelates them otherwise.

`CREATE TABLE ... AS` statements are only pushed down as a whole when their query only uses constructs that are translated whatever types they operate on (columns, constants, comparisons, `count` / `min` / `max`, ...). Otherwise only the query is pushed down, and DuckDB inserts its result into the new table.

Known differences to executing the query in DuckDB:

* SQLite has no NaN - DuckDB results that are NaN (e.g. `0 / 0`, `sqrt(-1)`) raise an error.
* Integer overflows in arithmetic produce floating point numbers in SQLite (DuckDB raises an error). `sum` of integers raises an error when the sum does not fit into a 64-bit integer (DuckDB sums them as `HUGEINT`).
* Floating point results of statistical aggregates (e.g. `stddev`, `avg`) can differ in the last digits.
* Dates and timestamps with years before 1 or after 9999 cannot be represented and raise an error.
* Values whose storage class does not match the type of their column (e.g. the real `1.5` in an `INTEGER` column) are not rejected like the table scan rejects them.
* SQLite's string functions (e.g. `length`, `substr`) end strings at a NUL character.
* Duplicate result column names are made unique (e.g. `i` and `i_1`), like for the result of a subquery.
* Pushed down queries require a database that uses the UTF-8 encoding.

## Building & Loading the Extension

>**Tip**:
>
>Before building, fetch submodules with `make pull`

To build, type
```
# to build with multiple cores, use `make GEN=ninja release`
make
```

To run, run the bundled `duckdb` shell:
```
 ./build/release/duckdb -unsigned
```

Then, load the SQLite extension like so:
```SQL
LOAD 'build/release/extension/sqlite_scanner/sqlite_scanner.duckdb_extension';
```

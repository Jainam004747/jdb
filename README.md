# jdb

![CI](https://github.com/Jainam004747/jdb/actions/workflows/ci.yml/badge.svg)

jdb is a relational database engine written from scratch in C, about 3,400 lines with no dependencies. It stores tables in **B+trees** inside a single file of 4 KB pages, caches pages in a **buffer pool**, makes transactions atomic and crash-safe with a **write-ahead log**, and runs **SQL** through a hand-written parser and an iterator-based query executor.

Its output is checked against **SQLite**: every SQL test script must produce byte-for-byte the same results from both.

```
$ ./jdb shop.db
jdb> CREATE TABLE menu (id INTEGER PRIMARY KEY, name TEXT NOT NULL, price INTEGER, category TEXT);
jdb> INSERT INTO menu (name, price, category) VALUES ('coffee', 199, 'drink'), ('tea', 159, 'drink'),
...>   ('donut', 129, 'food'), ('bagel', 249, 'food');
(4 rows changed)
jdb> SELECT category, COUNT(*) AS items, SUM(price) AS total FROM menu GROUP BY category ORDER BY total DESC;
┌──────────┬───────┬───────┐
│ category │ items │ total │
├──────────┼───────┼───────┤
│ food     │     2 │   378 │
│ drink    │     2 │   358 │
└──────────┴───────┴───────┘
(2 rows)
jdb> EXPLAIN SELECT name FROM menu WHERE id BETWEEN 2 AND 3 ORDER BY name;
SEARCH menu USING INTEGER PRIMARY KEY (rowid 2..3)
FILTER
SORT for ORDER BY
```

## Features

- **Storage:** a single database file of 4 KB pages, with slotted leaf pages for variable-length rows.
- **B+tree** per table, keyed by rowid:
  - leaf and internal node splits;
  - linked leaves for range scans;
  - an append-optimised split for sequential ids, which halves file size;
  - a structural self-check (`.check`).
- **Buffer pool:** LRU eviction, pin counts, and hit/miss statistics (`.stats`).
- **Transactions:** `BEGIN` / `COMMIT` / `ROLLBACK`, plus autocommit for single statements.
  - The redo write-ahead log has CRC32-checksummed frames.
  - Crash recovery runs on startup, and torn writes are detected.
  - Statements are atomic: constraints are checked before anything is written.
- **SQL:**
  - `CREATE TABLE` / `DROP TABLE` with `INTEGER` / `TEXT` columns, `INTEGER PRIMARY KEY`, `NOT NULL`;
  - `INSERT` (multi-row), `UPDATE`, `DELETE`;
  - `SELECT` with `WHERE`, `GROUP BY`, `ORDER BY` (multiple keys, `ASC`/`DESC`, aliases, positions), `LIMIT` / `OFFSET`;
  - aggregates `COUNT`, `SUM`, `MIN`, `MAX`, and functions `LENGTH`, `UPPER`, `LOWER`, `ABS`, `TYPEOF`;
  - three-valued `NULL` logic, `BETWEEN`, `IS [NOT] NULL`, `||`;
  - `EXPLAIN`.
- **Query planner:** conditions on the primary key (`id = 5`, `id > 100`, `id BETWEEN a AND b`) turn a full scan into a B+tree range seek.
- **Shell:** boxed tables interactively, SQLite-compatible `a|b|c` output for scripts, and `.tables`, `.schema`, `.stats`, `.check`.

## Quick start

```bash
make                      # builds ./jdb
./jdb shop.db             # interactive shell (try .help)
./jdb shop.db < script.sql
./jdb shop.db -c "SELECT COUNT(*) FROM menu;"
make test                 # unit + SQLite differential + crash-recovery tests
./tests/bench.sh 100000   # timing comparison with SQLite
```

## Architecture

```
 SQL text ─▶ sql.c (tokenizer + recursive-descent parser) ─▶ Stmt tree
                                                               │
 exec.c   planner ─▶ Limit ◀─ Sort ◀─ Project/Aggregate ◀─ Filter ◀─ Scan (cursor)
                                                                       │
 btree.c  B+tree per table:  [internal: 40 | 80]                        │
                            /        |        \                         ▼
                       [leaf] ─▶ [leaf] ─▶ [leaf]     rows encoded by record.c
                                                                       │
 pager.c  buffer pool (LRU, pins) ─── COMMIT ──▶ WAL (fsync) ──▶ database file
```

Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) · design decisions: [docs/DECISIONS.md](docs/DECISIONS.md) · performance: [docs/BENCHMARKS.md](docs/BENCHMARKS.md) · build log: [docs/DEV-LOG.md](docs/DEV-LOG.md)

## Testing

| Suite | What it proves |
|---|---|
| `tests/test_btree.c` | 60,000 random inserts and 30,000 deletes with a 16-page buffer pool (constant eviction). After each phase: every key found, cursor order correct, B+tree invariants hold, and the data survives close/reopen. Rollback restores the exact previous state. |
| `tests/run_sql_tests.sh` | 12 SQL scripts (basics, NULL logic, ordering, aggregates, updates, transactions, constraints, 5,000-row workloads, Unicode) produce **identical output to SQLite** and the same number of errors. |
| `tests/crash_test.sh` | Simulated power failures inside `COMMIT`. Mid-WAL, the transaction is gone; after the WAL fsync, it is recovered; with a torn final frame, it is ignored. The B+tree is intact in every case. |
| `make asan` | All of the above under AddressSanitizer and UBSan. |

## Known limitations

- **Types:** only `INTEGER` and `TEXT` (no floating point, so no `AVG`), and only the `INTEGER PRIMARY KEY` index (no secondary indexes or joins).
- **Single writer:** one process at a time, enforced with a file lock. There is no concurrent access.
- **No space reuse:** deleted rows leave space inside pages that later inserts reuse, but underfull pages are not merged, and dropped tables' pages are zeroed rather than put on a free list.
- **Transactions:** a transaction's dirty pages must fit in memory (no-steal); the buffer pool grows temporarily if needed.

## What I learned

- **Why databases use B+trees.** High fan-out (340 children per node) makes 100,000 rows only 3 levels deep, and linked leaves make range scans sequential.
- **Atomicity and durability come from ordering writes.** WAL first, fsync, then the database file. Crash tests at each point prove it.
- **Split strategy matters.** A 50/50 split with sequential ids left every page half empty. Measuring file size found it, and an append-optimised split halved the file.
- **Iterators keep memory flat.** Only `ORDER BY` and `GROUP BY` need to materialise rows.

Developed with AI assistance (Claude) as a learning project.

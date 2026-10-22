# Development Log

## 2026-10-21 - Building jdb

### Tools
```bash
sudo apt-get install -y sqlite3 bc    # SQLite is the reference implementation for tests
make && make test && make asan
```

### Build order (one commit each)
1. `util.c`: arena allocator and CRC32.
2. `pager.c`: page I/O, buffer pool, and WAL with crash injection points (`JDB_CRASH`).
3. `btree.c`: slotted leaves, splits, cursors, self-check. Tested by `tests/test_btree.c` before any SQL existed.
4. `record.c` + `sql.c`: row encoding, catalog, SQL parser.
5. `exec.c` + `main.c`: planner, operators, shell.
6. Tests against SQLite, crash tests, benchmarks.

### Bugs and findings
- **`LENGTH('crème brûlée ☕')`** returned 28 (bytes) where SQLite returns 23 (characters). Fixed by counting UTF-8 lead bytes. Found by differential testing.
- **Error counting in the test runner.** SQLite prints multi-line error messages, so the runner now counts only `Parse error` / `Runtime error` lines.
- **File size 3.3× SQLite's.** Half-empty pages from 50/50 splits with sequential keys; fixed with the append-optimised split (see BENCHMARKS.md).
- **A comment ending in a backslash** (`\` in the ASCII diagram in btree.c) swallowed the next line. gcc's `-Wcomment` caught it.

### Crash testing approach
`JDB_CRASH=mid-wal` and `JDB_CRASH=after-wal` make the process `_exit()` at exact points inside `pager_commit`, which simulates a power failure deterministically. `tests/crash_test.sh` checks atomicity (mid-WAL: the transaction is gone), durability (after the WAL fsync: the transaction is recovered) and torn-write detection (truncated WAL: the transaction is ignored).

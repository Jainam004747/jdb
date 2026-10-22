# Design Decisions

### D-001: One B+tree per table, keyed by a 64-bit rowid
- **Why:** This is SQLite's design. An `INTEGER PRIMARY KEY` is the rowid itself, so lookups by primary key are a single tree descent with no separate index.
- **Trade-offs:** Secondary indexes (separate trees mapping value → rowid) are not implemented.

### D-002: Slotted pages for leaves
- **Options:** Fixed-size rows; slotted pages.
- **Why:** Rows vary in size (TEXT). With slots, the sorted order lives in a small array of 2-byte offsets, so inserting in the middle moves 2·n bytes, not whole rows.

### D-003: No-steal / redo-only write-ahead log
- **Options:** An undo log (rollback journal, like old SQLite), a redo WAL, or ARIES (steal + undo/redo).
- **Decision:** Dirty pages stay in memory until COMMIT; COMMIT writes them to the WAL, fsyncs, then copies them into the database.
- **Why:** The simplest scheme with real atomicity and durability. ROLLBACK is free (drop dirty pages) and recovery is "redo committed transactions".
- **Trade-offs:** A transaction's changes must fit in memory, and every commit writes pages twice (WAL + database). ARIES removes both limits at much higher complexity.

### D-004: CRC32 on every WAL frame
- **Why:** A power failure can leave a half-written frame. Without a checksum, recovery could apply garbage. The crash tests truncate the WAL to prove torn frames are ignored.

### D-005: Statement atomicity by validating before writing
- **Context:** Undoing a half-applied statement inside an explicit transaction would need savepoints.
- **Decision:** INSERT checks every row (types, NOT NULL, duplicate keys, including duplicates within the same statement) before inserting any. UPDATE computes every new row before writing.
- **Why:** A failed statement changes nothing, with no undo machinery.

### D-006: Append-optimised leaf split
- **Context:** The benchmark showed jdb files were 3.3× larger than SQLite's for sequential ids.
- **Cause:** A 50/50 split always leaves the left page half full, and with increasing keys nothing is ever inserted there again.
- **Decision:** When the new key goes past the end of the rightmost leaf, keep the old page full and start a new one.
- **Result:** File size halved (8.9 MB → 4.5 MB for 100k rows). SQLite applies the same idea ("quick balance").

### D-007: The iterator (volcano) execution model
- **Why:** Each operator is small and independent, and rows stream through, so memory stays flat unless sorting or grouping is required. Most databases (PostgreSQL, SQLite's VDBE in spirit) work this way.
- **Trade-offs:** One function call per row per operator. Vectorised engines process batches instead.

### D-008: Arena allocation per statement
- **Why:** A statement allocates many small objects (AST nodes, values, strings) that all die together. One `arena_free` replaces hundreds of `free` calls and makes leaks impossible.

### D-009: SQLite as the test oracle
- **Why:** Differential testing finds bugs no hand-written expectation would, such as UTF-8 `LENGTH`, NULL ordering in `ORDER BY`, and integer division semantics. It also forces SQLite-compatible behaviour, including type affinity (`'25'` stored into an INTEGER column becomes 25).

### D-010: Store the CREATE TABLE text in the catalog
- **Why:** The schema is re-parsed on open with the same parser, so there is no second schema format to keep in sync. SQLite's `sqlite_schema` table does the same.

# jdb Architecture

## Layers

| File | Responsibility | Depends on |
|---|---|---|
| `util.c` | Arena allocator, CRC32, fatal errors | none |
| `pager.c` | Pages ↔ file, buffer pool, WAL, transactions, crash recovery | util |
| `btree.c` | B+tree: insert/split, delete, find, cursors, self-check | pager |
| `record.c` | Value types, row encoding, catalog on page 0 | pager, sql (to re-parse schemas) |
| `sql.c` | Tokenizer and recursive-descent parser → `Stmt` | util |
| `exec.c` | Binding, planning, iterator operators, output formatting | all of the above |
| `main.c` | Shell: line editing, dot-commands, script mode | exec |

## File format

```
page 0     [ "JDB1" | version | page count | ... | catalog at offset 64 ]
           catalog: u16 count, then per table: u32 root page, u16 length, CREATE TABLE text
page 1..N  B+tree nodes (leaf or internal)
<db>-wal   write-ahead log: frames of (header + 4 KB page) and COMMIT records
```

## B+tree pages

```
offset 0  u8  type (1 leaf, 2 internal)
       2  u16 cell count
       4  u16 start of cell content (leaf)
       8  u32 next leaf (leaf) / rightmost child (internal)
      12  leaf: u16 slot array, sorted by key → cells at the end of the page
          internal: (u32 child, i64 key) pairs; child i holds keys < key i
```

**Slotted pages.** The slot array grows from the front and the cell data grows from the back. Inserting into the middle only shifts 2-byte slots, never row data. Deleting removes the slot. The hole it leaves is reclaimed by defragmentation the next time the page needs space.

**Insert:**

1. Descend to the leaf.
2. If the cell fits, insert it into the slot array in sorted order.
3. Otherwise split the leaf: by size into two halves, or, when appending past the last key, keep the old page full. The first key of the new page goes up to the parent as a separator.
4. Full internal nodes split around their middle key, which moves up.
5. When the root splits, its contents move to a new page, so the root's page number, recorded in the catalog, never changes.

**Search** is a binary search inside each node, so a lookup costs `depth × log2(cells)` comparisons and `depth` page reads. That is 3 for 100,000 rows.

## Buffer pool

- **Frames:** each holds `{pgno, data, dirty, pins, last_used}`, found through a 1024-bucket hash table.
- **`pager_get`:** pins the page. Callers must `pager_unpin` it when done.
- **Eviction:** picks the least recently used frame that is unpinned **and clean**.
- **No-steal:** dirty pages are never evicted before COMMIT, so the pool may temporarily exceed its capacity inside a big transaction.

## Transactions and recovery

```
COMMIT:  dirty pages ──▶ WAL frames ──▶ COMMIT frame ──▶ fsync(WAL) ══ commit point ══
         ──▶ write pages into the database file ──▶ fsync(db) ──▶ truncate WAL

ROLLBACK: drop dirty frames, re-read the page count from page 0 on disk
RECOVERY: on open, replay every WAL transaction that has a valid COMMIT frame; ignore the rest
```

Each frame carries a CRC32 of its header and page. A crash in the middle of a write leaves a frame whose checksum does not match. Recovery stops there, so an incomplete transaction is never applied.

## Query execution

**Binding** resolves column names to indexes and checks that aggregates are only used where allowed.

**Planning** walks the top-level `AND` terms of `WHERE`, looking for `pk = c`, `pk < c`, `c <= pk` and so on, and narrows a `[lo, hi]` key range. The scan seeks to `lo` and stops after `hi`. The full `WHERE` is still evaluated on every row, so the plan can never change the answer, only the amount of work.

**Operators.** Each has `next(op, &row)`:

| Operator | Streams? | Notes |
|---|---|---|
| Scan | yes | B+tree cursor; decodes into a per-row scratch arena |
| Filter | yes | evaluates `WHERE` with three-valued logic |
| Project | yes | evaluates the select list plus ORDER BY keys |
| Aggregate | no | loads input, stable-sorts by GROUP BY keys, emits one row per group |
| Sort | no | stable merge sort on the ORDER BY keys |
| Limit | yes | OFFSET then LIMIT |

**UPDATE / DELETE** first collect the matching rows, then change the tree. Modifying a B+tree while a cursor walks it could skip or revisit rows (the "Halloween problem").

**Memory.** Each statement gets an arena that holds its parse tree, copied rows and strings, and it is freed in one call when the statement ends. Scans reset a per-row scratch arena on every row, so `SELECT *` over a million rows uses constant memory.

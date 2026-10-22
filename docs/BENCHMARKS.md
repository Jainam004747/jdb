# Benchmarks

Produced by `tests/bench.sh 100000` on a 2-vCPU Linux container, with each workload run as a separate process for both engines. SQLite 3.45 uses its default settings.

| Workload (100,000 rows) | jdb | SQLite |
|---|---:|---:|
| Bulk insert, one transaction | 0.20 s | 0.15 s |
| 2,000 primary-key lookups | 0.011 s | 0.023 s |
| Range scan (1,001 rows by primary key) | 0.004 s | 0.004 s |
| Full scan + filter | 0.015 s | 0.012 s |
| GROUP BY over all rows | 0.067 s | 0.033 s |
| File size | 4.5 MB | 2.7 MB |
| B+tree depth | 3 | (n/a) |

These are timings of tiny workloads, so process start-up is a large part of every number. jdb is in the same order of magnitude as SQLite on lookups and scans, which says more about B+trees than about jdb.

## What the numbers taught

1. **Split strategy and file size.** The first version produced an **8.9 MB** file. With sequential ids, every 50/50 leaf split left a half-empty page that never received another key. The append-optimised split ([D-006](DECISIONS.md)) cut it to **4.5 MB** and reduced insert time by about 10%.
2. **The remaining size gap.** jdb stores integers as fixed 8 bytes, and every TEXT value carries a 2-byte length. SQLite uses variable-length integers (1–9 bytes) and a compact record header. A varint encoding would be the next optimisation.
3. **GROUP BY is about 2× slower.** jdb sorts all rows to form groups. A hash aggregate would avoid the O(n log n) sort when the number of groups is small.
4. **The planner matters.** `WHERE id BETWEEN 5000 AND 6000` reads about 10 leaf pages instead of all 1,100; `EXPLAIN` shows `SEARCH ... USING INTEGER PRIMARY KEY`.

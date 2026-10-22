#!/bin/bash
# Times the same workload on jdb and SQLite. Usage: tests/bench.sh [rows]
set -eu
cd "$(dirname "$0")/.."
ROWS=${1:-100000}
W=$(mktemp -d)
python3 - "$ROWS" > "$W/load.sql" <<'PY'
import sys
n = int(sys.argv[1])
print("CREATE TABLE orders (id INTEGER PRIMARY KEY, customer TEXT, item TEXT, qty INTEGER);")
print("BEGIN;")
items = ["coffee", "tea", "donut", "bagel", "muffin"]
for start in range(1, n + 1, 100):
    vals = ", ".join(f"({i}, 'customer{i % 997}', '{items[i % 5]}', {i % 13})" for i in range(start, min(start + 100, n + 1)))
    print(f"INSERT INTO orders VALUES {vals};")
print("COMMIT;")
PY
python3 - "$ROWS" > "$W/lookups.sql" <<'PY'
import sys, random
n = int(sys.argv[1]); random.seed(1)
for _ in range(2000):
    print(f"SELECT qty FROM orders WHERE id = {random.randint(1, n)};")
PY
echo "SELECT COUNT(*) FROM orders WHERE id BETWEEN 5000 AND 6000;" > "$W/range.sql"
echo "SELECT item, COUNT(*), SUM(qty) FROM orders GROUP BY item;" > "$W/group.sql"
echo "SELECT COUNT(*) FROM orders WHERE qty = 7;" > "$W/scan.sql"

t() { local s=$(date +%s.%N); "$@" > /dev/null; echo "$(date +%s.%N) - $s" | bc; }
printf "%-34s %10s %10s\n" "workload ($ROWS rows)" "jdb (s)" "sqlite (s)"
row() { printf "%-34s %10.3f %10.3f\n" "$1" "$2" "$3"; }
row "bulk insert (1 transaction)" "$(t ./jdb "$W/j.db" < "$W/load.sql")" "$(t sqlite3 "$W/s.db" < "$W/load.sql")"
row "2000 primary-key lookups" "$(t ./jdb "$W/j.db" < "$W/lookups.sql")" "$(t sqlite3 "$W/s.db" < "$W/lookups.sql")"
row "range scan (1001 rows)" "$(t ./jdb "$W/j.db" < "$W/range.sql")" "$(t sqlite3 "$W/s.db" < "$W/range.sql")"
row "full scan + filter" "$(t ./jdb "$W/j.db" < "$W/scan.sql")" "$(t sqlite3 "$W/s.db" < "$W/scan.sql")"
row "GROUP BY over all rows" "$(t ./jdb "$W/j.db" < "$W/group.sql")" "$(t sqlite3 "$W/s.db" < "$W/group.sql")"
echo
echo "file size: jdb $(du -k "$W/j.db" | cut -f1) KB, sqlite $(du -k "$W/s.db" | cut -f1) KB"
echo ".stats" | ./jdb "$W/j.db" | grep -E "depth|pages in file"
rm -rf "$W"

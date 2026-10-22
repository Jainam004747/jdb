#!/bin/bash
# Crash-recovery tests. JDB_CRASH makes jdb "lose power" at a chosen point inside COMMIT:
#   mid-wal    half of the transaction's pages are in the WAL, no COMMIT record
#              -> after restart the transaction must be GONE (atomicity)
#   after-wal  the WAL holds the whole transaction and was fsynced, the database file was
#              not updated yet -> after restart the transaction must be THERE (durability)
set -u
cd "$(dirname "$0")/.."
DB=$(mktemp -u /tmp/jdb-crash-XXXXXX)
fail=0

check() { # description, expected, actual
  if [ "$2" == "$3" ]; then echo "  ok: $1"; else echo "  FAIL: $1 (expected '$2', got '$3')"; fail=1; fi
}

setup() {
  rm -f "$DB" "$DB-wal"
  ./jdb "$DB" -c "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT); INSERT INTO t (v) VALUES ('committed');"
}

# A transaction big enough to touch many pages, so a crash can land in the middle of it.
BIG="BEGIN;"
for i in $(seq 1 400); do BIG="$BIG INSERT INTO t (v) VALUES ('row $i with some padding to fill pages');"; done
BIG="$BIG COMMIT;"

echo "crash in the middle of writing the WAL:"
setup
JDB_CRASH=mid-wal ./jdb "$DB" -c "$BIG" 2>/dev/null
check "process died before commit" "3" "$?"
check "WAL left behind" "yes" "$([ -s "$DB-wal" ] && echo yes || echo no)"
check "partial transaction rolled back" "1" "$(./jdb "$DB" -c 'SELECT COUNT(*) FROM t;' 2>/dev/null)"
check "B+tree intact" "t: ok (1 rows)" "$(echo .check | ./jdb "$DB")"

echo "crash after the WAL was fsynced, before the database file was written:"
setup
JDB_CRASH=after-wal ./jdb "$DB" -c "$BIG" 2>/dev/null
check "process died after commit point" "3" "$?"
recovered=$(./jdb "$DB" -c 'SELECT COUNT(*) FROM t;' 2>&1)
check "committed transaction recovered from the WAL" "jdb: recovered 1 committed transaction from the write-ahead log
401" "$recovered"
check "B+tree intact" "t: ok (401 rows)" "$(echo .check | ./jdb "$DB")"

echo "torn WAL frame (last bytes of the file corrupted):"
setup
JDB_CRASH=after-wal ./jdb "$DB" -c "$BIG" 2>/dev/null
size=$(stat -c %s "$DB-wal")
truncate -s $((size - 10)) "$DB-wal"   # the COMMIT record is now incomplete
check "torn transaction ignored" "1" "$(./jdb "$DB" -c 'SELECT COUNT(*) FROM t;' 2>/dev/null)"

rm -f "$DB" "$DB-wal"
if [ $fail -eq 0 ]; then echo "crash tests: all passed"; fi
exit $fail

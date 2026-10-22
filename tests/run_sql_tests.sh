#!/bin/bash
# Differential testing against SQLite: each tests/sql/*.sql script is run by sqlite3 and by jdb
# on fresh databases. Their standard output (list format: a|b|c) must be identical.
# Error messages go to stderr and are compared only by count: both must reject the same number
# of statements.
set -u
cd "$(dirname "$0")/.."
OUT=tests/out
mkdir -p "$OUT"
pass=0
fail=0
for sql in tests/sql/*.sql; do
  name=$(basename "$sql" .sql)
  rm -f "$OUT/$name.db" "$OUT/$name.db-wal"
  sqlite3 :memory: < "$sql" > "$OUT/$name.expected" 2> "$OUT/$name.expected.err"
  ./jdb "$OUT/$name.db" < "$sql" > "$OUT/$name.actual" 2> "$OUT/$name.actual.err"
  want_errs=$(grep -cE "^(Parse|Runtime) error" "$OUT/$name.expected.err")
  got_errs=$(grep -c "^Error" "$OUT/$name.actual.err")
  if ! cmp -s "$OUT/$name.expected" "$OUT/$name.actual"; then
    echo "FAIL $name: output differs from SQLite"
    diff "$OUT/$name.expected" "$OUT/$name.actual" | head -12 | sed 's/^/     /'
    fail=$((fail + 1))
  elif [ "$want_errs" != "$got_errs" ]; then
    echo "FAIL $name: SQLite reported $want_errs error(s), jdb reported $got_errs"
    sed 's/^/     sqlite: /' "$OUT/$name.expected.err" | head -5
    sed 's/^/     jdb:    /' "$OUT/$name.actual.err" | head -5
    fail=$((fail + 1))
  else
    pass=$((pass + 1))
  fi
  # Every database jdb leaves behind must pass the structural check.
  if ! ./jdb "$OUT/$name.db" -c "" < /dev/null > /dev/null || ! echo ".check" | ./jdb "$OUT/$name.db" > "$OUT/$name.check"; then
    echo "FAIL $name: B+tree check failed"; cat "$OUT/$name.check"; fail=$((fail + 1))
  fi
done
echo "SQL tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

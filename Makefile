CFLAGS = -std=gnu11 -g -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
SRCS = src/util.c src/pager.c src/btree.c src/record.c src/sql.c src/exec.c
OBJS = $(SRCS:.c=.o)

all: jdb tests/test_btree

jdb: $(OBJS) src/main.o
	$(CC) $(CFLAGS) -o $@ $^

tests/test_btree: $(OBJS) tests/test_btree.c
	$(CC) $(CFLAGS) -Isrc -o $@ $^

$(OBJS) src/main.o: src/jdb.h

# Unit tests (B+tree, pager), SQL tests compared against SQLite, and crash-recovery tests.
test: all
	./tests/test_btree
	./tests/run_sql_tests.sh
	./tests/crash_test.sh

asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -O0 -fsanitize=address,undefined" test

clean:
	rm -f jdb tests/test_btree src/*.o
	rm -rf tests/out

.PHONY: all test asan clean

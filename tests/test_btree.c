// Unit tests for the pager and the B+tree, independent of SQL.
//
// The main test inserts and deletes many random keys, and after each phase checks the tree
// against a simple model (a sorted array) and runs btree_check(), which verifies the B+tree
// invariants: sorted keys, correct separator keys, every leaf at the same depth, and sibling
// links in order. A tiny buffer pool forces constant eviction, so the pager is tested too.

#include "jdb.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                                                            \
  do {                                                                                              \
    if (!(cond)) {                                                                                  \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                          \
      fprintf(stderr, __VA_ARGS__);                                                                 \
      fprintf(stderr, "\n");                                                                        \
      failures++;                                                                                   \
    }                                                                                               \
  } while (0)

static uint64_t rng = 88172645463325252ULL;
static uint64_t next_rand(void) { // xorshift64: fast and reproducible
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}

static int make_value(int64_t key, uint8_t *buf) {
  int len = 1 + (int)(key % 120); // varied sizes exercise the slotted pages
  for (int i = 0; i < len; i++)
    buf[i] = (uint8_t)(key * 31 + i);
  return len;
}

static void verify(BTree *t, int64_t *keys, int n, const char *phase) {
  long count;
  char err[256] = "";
  CHECK(btree_check(t, &count, err, sizeof err), "%s: structure check failed: %s", phase, err);
  CHECK(count == n, "%s: tree has %ld keys, expected %d", phase, count, n);
  uint8_t buf[MAX_ROW_SIZE], want[MAX_ROW_SIZE];
  for (int i = 0; i < n; i += 7) {
    int len;
    bool found = btree_find(t, keys[i], buf, &len);
    int wlen = make_value(keys[i], want);
    CHECK(found && len == wlen && memcmp(buf, want, (size_t)len) == 0, "%s: key %lld lost or wrong",
          phase, (long long)keys[i]);
  }
  // A cursor must visit exactly the sorted keys.
  Cursor c;
  int i = 0;
  for (btree_first(t, &c); c.valid; btree_next(&c), i++) {
    if (i < n)
      CHECK(cursor_key(&c) == keys[i], "%s: cursor at %d gave %lld, expected %lld", phase, i,
            (long long)cursor_key(&c), (long long)keys[i]);
  }
  CHECK(i == n, "%s: cursor visited %d keys, expected %d", phase, i, n);
}

static int cmp64(const void *a, const void *b) {
  int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
  return x < y ? -1 : x > y;
}

static void test_random_inserts_and_deletes(const char *path) {
  enum { N = 60000 };
  Pager *p = pager_open(path, 16); // tiny pool: forces eviction constantly
  Pgno root = btree_create(p);
  pager_commit(p);
  BTree t = {p, root};

  int64_t *keys = malloc(sizeof(int64_t) * N);
  int n = 0;
  uint8_t buf[MAX_ROW_SIZE];
  while (n < N) {
    int64_t k = (int64_t)(next_rand() % 1000000);
    int len = make_value(k, buf);
    if (btree_insert(&t, k, buf, len))
      keys[n++] = k;
    else
      CHECK(btree_find(&t, k, NULL, NULL), "insert of %lld refused but key not present", (long long)k);
    if (n % 5000 == 0)
      pager_commit(p);
  }
  pager_commit(p);
  qsort(keys, N, sizeof *keys, cmp64);
  verify(&t, keys, n, "after inserts");
  CHECK(btree_depth(&t) >= 3, "expected a multi-level tree, depth is %d", btree_depth(&t));

  // Delete every other key.
  int kept = 0;
  for (int i = 0; i < n; i++) {
    if (i % 2 == 0)
      CHECK(btree_delete(&t, keys[i]), "delete of %lld failed", (long long)keys[i]);
    else
      keys[kept++] = keys[i];
  }
  CHECK(!btree_delete(&t, -1), "deleting a missing key should return false");
  pager_commit(p);
  verify(&t, keys, kept, "after deletes");

  // Seeking lands on the first key >= the target.
  Cursor c;
  btree_seek(&t, keys[100] - 1, &c);
  CHECK(c.valid && cursor_key(&c) == keys[100], "seek did not find the next key");
  CHECK(btree_max_key(&t) == keys[kept - 1], "max key wrong");
  pager_close(p);

  // Everything must still be there after closing and reopening the file.
  p = pager_open(path, 64);
  BTree t2 = {p, root};
  verify(&t2, keys, kept, "after reopen");
  pager_close(p);
  free(keys);
}

static void test_rollback(const char *path) {
  Pager *p = pager_open(path, 32);
  Pgno root = btree_create(p);
  pager_commit(p);
  BTree t = {p, root};
  uint8_t v[4] = {1, 2, 3, 4};
  btree_insert(&t, 1, v, 4);
  pager_commit(p);

  Pgno pages_before = p->page_count;
  pager_begin(p);
  for (int64_t k = 2; k < 3000; k++)
    btree_insert(&t, k, v, 4); // enough to split pages and allocate new ones
  pager_rollback(p);
  CHECK(p->page_count == pages_before, "rollback should restore the page count");
  CHECK(btree_find(&t, 1, NULL, NULL), "committed key lost by rollback");
  CHECK(!btree_find(&t, 2, NULL, NULL), "rolled-back key still visible");
  long count;
  char err[256];
  CHECK(btree_check(&t, &count, err, sizeof err) && count == 1, "tree damaged by rollback");
  pager_close(p);
}

static void test_records(void) {
  Arena a = {0};
  Value in[4] = {v_int(-42), v_null(), v_text("crème brûlée"), v_int(INT64_MAX)};
  uint8_t buf[256];
  int len = record_encode(in, 4, buf, sizeof buf);
  CHECK(len > 0, "encode failed");
  Value out[5];
  record_decode(&a, buf, len, out, 5);
  for (int i = 0; i < 4; i++)
    CHECK(value_compare(in[i], out[i]) == 0, "value %d changed in a round trip", i);
  CHECK(out[4].type == VAL_NULL, "missing trailing column should decode as NULL");
  CHECK(record_encode(in, 4, buf, 10) == -1, "encoding into a small buffer must fail");
  CHECK(value_compare(v_null(), v_int(0)) < 0 && value_compare(v_int(99), v_text("1")) < 0,
        "type order should be NULL < INTEGER < TEXT");
  arena_free(&a);
}

int main(void) {
  char path[] = "/tmp/jdb-test-XXXXXX";
  int fd = mkstemp(path);
  close(fd);
  unlink(path);
  char wal[64];
  snprintf(wal, sizeof wal, "%s-wal", path);

  test_records();
  test_random_inserts_and_deletes(path);
  unlink(path);
  unlink(wal);
  test_rollback(path);
  unlink(path);
  unlink(wal);

  if (failures) {
    fprintf(stderr, "test_btree: %d failure(s)\n", failures);
    return 1;
  }
  printf("test_btree: all checks passed\n");
  return 0;
}

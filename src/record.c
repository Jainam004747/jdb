// Values, rows and the catalog.
//
// A ROW is stored in its table's B+tree as bytes:
//   u8 column count, then for each column:  u8 type (0 NULL, 1 INTEGER, 2 TEXT)
//                                            INTEGER: 8 bytes   TEXT: u16 length + bytes
//
// The CATALOG (the list of tables) lives on page 0 after the file header:
//   offset 64: u16 table count, then per table: u32 root page, u16 length, CREATE TABLE text
// Storing the original CREATE TABLE text and re-parsing it on open is what SQLite does too: the
// schema format can never get out of sync with the SQL parser.

#include "jdb.h"

#include <stdlib.h>
#include <string.h>

#define CATALOG_OFFSET 64

Value v_null(void) { return (Value){.type = VAL_NULL}; }
Value v_int(int64_t i) { return (Value){.type = VAL_INT, .i = i}; }
Value v_text(const char *s) { return (Value){.type = VAL_TEXT, .s = s, .len = (int)strlen(s)}; }

// Sort order across types matches SQLite: NULL < INTEGER < TEXT.
int value_compare(Value a, Value b) {
  if (a.type != b.type)
    return a.type < b.type ? -1 : 1;
  switch (a.type) {
  case VAL_NULL:
    return 0;
  case VAL_INT:
    return a.i < b.i ? -1 : a.i > b.i;
  case VAL_TEXT: {
    int n = a.len < b.len ? a.len : b.len;
    int c = memcmp(a.s, b.s, (size_t)n);
    if (c)
      return c < 0 ? -1 : 1;
    return a.len < b.len ? -1 : a.len > b.len;
  }
  }
  return 0;
}

bool value_truthy(Value v) {
  if (v.type == VAL_INT)
    return v.i != 0;
  if (v.type == VAL_TEXT)
    return atoll(v.s) != 0; // SQLite converts text to a number in a boolean context
  return false;            // NULL is not true
}

int record_encode(Value *vals, int n, uint8_t *buf, int cap) {
  int pos = 0;
  if (cap < 1)
    return -1;
  buf[pos++] = (uint8_t)n;
  for (int i = 0; i < n; i++) {
    Value v = vals[i];
    if (pos + 1 > cap)
      return -1;
    buf[pos++] = (uint8_t)v.type;
    if (v.type == VAL_INT) {
      if (pos + 8 > cap)
        return -1;
      memcpy(buf + pos, &v.i, 8);
      pos += 8;
    } else if (v.type == VAL_TEXT) {
      if (v.len > 65535 || pos + 2 + v.len > cap)
        return -1;
      uint16_t len = (uint16_t)v.len;
      memcpy(buf + pos, &len, 2);
      memcpy(buf + pos + 2, v.s, (size_t)v.len);
      pos += 2 + v.len;
    }
  }
  return pos;
}

void record_decode(Arena *a, const uint8_t *buf, int len, Value *vals, int n) {
  int stored = len > 0 ? buf[0] : 0;
  int pos = 1;
  for (int i = 0; i < n; i++) {
    if (i >= stored) { // a column the row does not have: NULL
      vals[i] = v_null();
      continue;
    }
    uint8_t type = buf[pos++];
    if (type == VAL_INT) {
      int64_t x;
      memcpy(&x, buf + pos, 8);
      pos += 8;
      vals[i] = v_int(x);
    } else if (type == VAL_TEXT) {
      uint16_t l;
      memcpy(&l, buf + pos, 2);
      char *s = arena_strndup(a, (const char *)buf + pos + 2, l);
      pos += 2 + l;
      vals[i] = (Value){.type = VAL_TEXT, .s = s, .len = l};
    } else {
      vals[i] = v_null();
    }
  }
}

// ---- catalog ------------------------------------------------------------------------------

Table *db_find_table(Database *db, const char *name) {
  for (int i = 0; i < db->ntables; i++)
    if (strcasecmp(db->tables[i]->name, name) == 0)
      return db->tables[i];
  return NULL;
}

void catalog_save(Database *db) {
  uint8_t *pg = pager_get(db->pager, 0);
  uint8_t *p = pg + CATALOG_OFFSET;
  memset(p, 0, PAGE_SIZE - CATALOG_OFFSET);
  uint16_t n = (uint16_t)db->ntables;
  memcpy(p, &n, 2);
  p += 2;
  for (int i = 0; i < db->ntables; i++) {
    Table *t = db->tables[i];
    uint16_t len = (uint16_t)strlen(t->sql);
    if (p + 6 + len > pg + PAGE_SIZE)
      die("the catalog is full (too many or too long CREATE TABLE statements)");
    memcpy(p, &t->root, 4);
    memcpy(p + 4, &len, 2);
    memcpy(p + 6, t->sql, len);
    p += 6 + len;
  }
  pager_mark_dirty(db->pager, 0);
  pager_unpin(db->pager, 0);
}

static Table *table_from_create(const char *sql, Pgno root) {
  Arena a = {0};
  const char *cursor = sql;
  char *err = NULL;
  Stmt *st = parse_statement(&a, &cursor, &err);
  if (!st || st->kind != ST_CREATE)
    die("corrupt catalog entry: %s", sql);
  Table *t = calloc(1, sizeof(Table));
  snprintf(t->name, sizeof t->name, "%s", st->table);
  t->ncols = st->ncols;
  memcpy(t->cols, st->cols, sizeof(Column) * (size_t)st->ncols);
  t->pk = -1;
  for (int i = 0; i < t->ncols; i++)
    if (t->cols[i].primary_key)
      t->pk = i;
  t->root = root;
  t->sql = strdup(sql);
  arena_free(&a);
  return t;
}

void catalog_load(Database *db) {
  uint8_t *pg = pager_get(db->pager, 0);
  uint8_t copy[PAGE_SIZE];
  memcpy(copy, pg, PAGE_SIZE);
  pager_unpin(db->pager, 0);
  const uint8_t *p = copy + CATALOG_OFFSET;
  uint16_t n;
  memcpy(&n, p, 2);
  p += 2;
  for (int i = 0; i < n; i++) {
    Pgno root;
    uint16_t len;
    memcpy(&root, p, 4);
    memcpy(&len, p + 4, 2);
    char *sql = strndup((const char *)p + 6, len);
    db->tables[db->ntables++] = table_from_create(sql, root);
    free(sql);
    p += 6 + len;
  }
}

Database *db_open(const char *path, int cache_pages) {
  Database *db = calloc(1, sizeof(Database));
  db->pager = pager_open(path, cache_pages);
  catalog_load(db);
  return db;
}

void db_close(Database *db) {
  pager_close(db->pager);
  for (int i = 0; i < db->ntables; i++) {
    free(db->tables[i]->sql);
    free(db->tables[i]);
  }
  free(db);
}

// jdb: a small relational database engine, written from scratch.
//
// Layers, bottom to top (each file depends only on the ones above it in this list):
//
//   util.c     arena allocator, CRC32, string helpers
//   pager.c    the database file split into 4 KB pages, a buffer pool caching them in memory,
//              and the write-ahead log (WAL) that makes commits atomic and crash-safe
//   btree.c    a B+tree stored in pages: one tree per table, keyed by rowid
//   record.c   values and rows <-> bytes, table schemas, the catalog
//   sql.c      SQL text -> tokens -> statement syntax trees
//   exec.c     query planning and execution with iterator ("volcano") operators
//   main.c     the interactive shell (REPL)

#ifndef JDB_H
#define JDB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PAGE_SIZE 4096
#define MAX_COLUMNS 32
#define MAX_TABLES 64
#define MAX_ROW_SIZE 1000   // keeps at least 4 rows per leaf page, so splits always work

// ---------------------------------------------------------------------------
// util.c
// ---------------------------------------------------------------------------

// An arena hands out memory from big blocks and frees it all at once. Every SQL statement
// gets one: parse trees, temporary rows and strings all die together when it finishes.
typedef struct ArenaBlock ArenaBlock;
typedef struct {
  ArenaBlock *head;
} Arena;

void *arena_alloc(Arena *a, size_t n);
char *arena_strndup(Arena *a, const char *s, size_t n);
char *arena_printf(Arena *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void arena_free(Arena *a);
uint32_t crc32(uint32_t crc, const void *data, size_t n);
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

// ---------------------------------------------------------------------------
// pager.c: pages, buffer pool, write-ahead log, transactions
// ---------------------------------------------------------------------------

typedef uint32_t Pgno; // page number; page 0 is the file header + catalog

typedef struct Frame {
  Pgno pgno;
  uint8_t data[PAGE_SIZE];
  bool dirty;      // changed since it was read from disk
  int pins;        // in use right now: must not be evicted
  uint64_t last_used; // for LRU eviction
  struct Frame *hash_next;
} Frame;

typedef struct {
  int fd;
  int wal_fd;
  char *path;
  char *wal_path;
  Pgno page_count;
  Frame **frames;     // the buffer pool
  int nframes;
  int capacity;       // soft limit; dirty pages may push past it (see pager.c)
  Frame *hash[1024];  // pgno -> frame lookup
  uint64_t clock;
  bool in_txn;        // inside an explicit BEGIN ... COMMIT
  uint64_t txn_id;
  // statistics, shown by .stats
  uint64_t reads, writes, hits, misses, evictions, commits, wal_frames;
} Pager;

Pager *pager_open(const char *path, int cache_pages);
void pager_close(Pager *p);
uint8_t *pager_get(Pager *p, Pgno pgno);        // pins the page
void pager_unpin(Pager *p, Pgno pgno);
void pager_mark_dirty(Pager *p, Pgno pgno);
Pgno pager_allocate(Pager *p);                  // returns a new, zeroed, pinned page
void pager_begin(Pager *p);
void pager_commit(Pager *p);
void pager_rollback(Pager *p);
int pager_recover(Pager *p);                    // replay committed WAL transactions; returns count

// ---------------------------------------------------------------------------
// btree.c: B+tree keyed by 64-bit rowid, values are byte strings (encoded rows)
// ---------------------------------------------------------------------------

typedef struct {
  Pager *pager;
  Pgno root;
} BTree;

typedef struct {
  BTree *tree;
  Pgno page;
  int slot;
  bool valid;
} Cursor;

Pgno btree_create(Pager *p);                       // a new empty tree; returns its root page
bool btree_insert(BTree *t, int64_t key, const uint8_t *val, int len); // false if key exists
bool btree_delete(BTree *t, int64_t key);
bool btree_find(BTree *t, int64_t key, uint8_t *out, int *len);
void btree_seek(BTree *t, int64_t key, Cursor *c);  // first entry with key >= key
void btree_first(BTree *t, Cursor *c);
void btree_next(Cursor *c);
int64_t cursor_key(Cursor *c);
const uint8_t *cursor_value(Cursor *c, int *len);
int64_t btree_max_key(BTree *t);                    // or 0 if empty
int btree_depth(BTree *t);
void btree_free_all(BTree *t);                      // DROP TABLE: give pages back (zeroed)
// Structural self-check used by tests: ordering, separator keys, sibling links.
bool btree_check(BTree *t, long *count, char *err, size_t errlen);

// ---------------------------------------------------------------------------
// record.c: values, rows, schemas, catalog
// ---------------------------------------------------------------------------

typedef enum { VAL_NULL, VAL_INT, VAL_TEXT } ValueType;

typedef struct {
  ValueType type;
  int64_t i;
  const char *s; // TEXT: NUL-terminated
  int len;
} Value;

typedef enum { COL_INTEGER, COL_TEXT } ColumnType;

typedef struct {
  char name[64];
  ColumnType type;
  bool primary_key;
  bool not_null;
} Column;

typedef struct {
  char name[64];
  int ncols;
  Column cols[MAX_COLUMNS];
  int pk;          // index of the INTEGER PRIMARY KEY column (an alias for rowid), or -1
  Pgno root;
  char *sql;       // CREATE TABLE text, stored in the catalog and re-parsed on open
} Table;

typedef struct {
  Pager *pager;
  Table *tables[MAX_TABLES];
  int ntables;
} Database;

Value v_null(void);
Value v_int(int64_t i);
Value v_text(const char *s);
int value_compare(Value a, Value b);  // NULL < INTEGER < TEXT, like SQLite
bool value_truthy(Value v);
int record_encode(Value *vals, int n, uint8_t *buf, int cap); // returns bytes, or -1 if too big
void record_decode(Arena *a, const uint8_t *buf, int len, Value *vals, int n);

Database *db_open(const char *path, int cache_pages);
void db_close(Database *db);
Table *db_find_table(Database *db, const char *name);
void catalog_save(Database *db);
void catalog_load(Database *db);

// ---------------------------------------------------------------------------
// sql.c: tokenizer + parser
// ---------------------------------------------------------------------------

typedef enum {
  EX_COLUMN, EX_INT, EX_TEXT, EX_NULL, EX_STAR,
  EX_BINARY, EX_UNARY, EX_IS_NULL, EX_FUNC,
} ExprKind;

typedef struct Expr Expr;
struct Expr {
  ExprKind kind;
  char op[4];          // "+", "=", "<=", "AND", "||", "NOT", "-" ...
  Expr *lhs, *rhs;
  char *name;          // column or function name
  int col;             // resolved column index (EX_COLUMN)
  int64_t ival;
  char *sval;
  bool negate;         // IS NOT NULL
  bool star;           // COUNT(*)
  int agg_slot;        // EX_FUNC: index into the aggregate state array
};

typedef enum {
  ST_CREATE, ST_DROP, ST_INSERT, ST_SELECT, ST_UPDATE, ST_DELETE,
  ST_BEGIN, ST_COMMIT, ST_ROLLBACK, ST_EXPLAIN,
} StmtKind;

typedef struct {
  Expr *expr;
  char *alias;
} SelectItem;

typedef struct {
  Expr *expr;
  bool desc;
} OrderItem;

typedef struct Stmt Stmt;
struct Stmt {
  StmtKind kind;
  char *table;
  // CREATE TABLE
  Column cols[MAX_COLUMNS];
  int ncols;
  bool if_not_exists;
  char *sql_text;
  // INSERT
  char *insert_cols[MAX_COLUMNS];
  int n_insert_cols;
  Expr ***rows;   // rows[r][c]
  int nrows;
  // SELECT
  SelectItem *items;
  int nitems;
  Expr *where;
  Expr **group_by;
  int ngroup;
  OrderItem *order_by;
  int norder;
  int64_t limit;  // -1 = no limit
  int64_t offset;
  // UPDATE
  char *set_cols[MAX_COLUMNS];
  Expr *set_vals[MAX_COLUMNS];
  int nset;
  // EXPLAIN
  Stmt *inner;
};

// Parses one statement from *sql, advancing past it. Returns NULL at end of input.
// On a syntax error returns NULL and sets *err.
Stmt *parse_statement(Arena *a, const char **sql, char **err);

// ---------------------------------------------------------------------------
// exec.c
// ---------------------------------------------------------------------------

typedef enum { OUT_LIST, OUT_TABLE } OutputMode;

typedef struct {
  OutputMode mode;
  bool headers;
  FILE *out;
} OutputOptions;

// Runs a statement. Returns 0 on success; on error writes a message to *err.
int execute(Database *db, Arena *a, Stmt *st, OutputOptions *opt, char **err);

#endif

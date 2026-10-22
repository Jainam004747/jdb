// Query execution.
//
// A SELECT is run as a pipeline of OPERATORS (the "volcano" or iterator model). Each operator
// has a next() function that returns one row at a time, pulling rows from the operator below:
//
//     Limit  <-  Sort  <-  Project / Aggregate  <-  Filter  <-  Scan (B+tree cursor)
//
// Rows stream through without being stored, unless an operator must see all of them first
// (Sort, Aggregate). That keeps memory flat for SELECT * FROM big_table.
//
// The PLANNER looks at the WHERE clause. If it constrains the INTEGER PRIMARY KEY (id = 5,
// id > 100, id BETWEEN 10 AND 20), the scan seeks straight to that range in the B+tree instead
// of reading the whole table. EXPLAIN shows which plan was chosen.

#include "jdb.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
  Value *v;
  int n;
  int64_t rowid;
} Row;

typedef struct {
  Database *db;
  Arena *arena;    // lives for the whole statement
  Arena row_arena; // scratch for one row; reset by the scan for each new row
  Table *table;
  char *err;
} Ctx;

#define FAIL(ctx, ...)                                                                              \
  do {                                                                                              \
    (ctx)->err = arena_printf((ctx)->arena, __VA_ARGS__);                                           \
    return -1;                                                                                      \
  } while (0)

// ---- binding: resolve column names to indexes ---------------------------------------------

#define ROWID_COLUMN (-2)

static int find_column(Table *t, const char *name) {
  for (int i = 0; i < t->ncols; i++)
    if (strcasecmp(t->cols[i].name, name) == 0)
      return i;
  if (strcasecmp(name, "rowid") == 0)
    return t->pk >= 0 ? t->pk : ROWID_COLUMN;
  return -1;
}

static bool is_aggregate_name(const char *name) {
  return !strcasecmp(name, "COUNT") || !strcasecmp(name, "SUM") || !strcasecmp(name, "MIN") ||
         !strcasecmp(name, "MAX") || !strcasecmp(name, "AVG");
}

static bool contains_aggregate(Expr *e) {
  if (!e)
    return false;
  if (e->kind == EX_FUNC && is_aggregate_name(e->name))
    return true;
  return contains_aggregate(e->lhs) || contains_aggregate(e->rhs);
}

static int bind(Ctx *ctx, Expr *e, bool allow_aggregates) {
  if (!e)
    return 0;
  switch (e->kind) {
  case EX_COLUMN:
    if (!ctx->table)
      FAIL(ctx, "no such column: %s", e->name);
    e->col = find_column(ctx->table, e->name);
    if (e->col == -1)
      FAIL(ctx, "no such column: %s", e->name);
    return 0;
  case EX_FUNC:
    if (is_aggregate_name(e->name)) {
      if (!allow_aggregates)
        FAIL(ctx, "misuse of aggregate function %s()", e->name);
      if (!strcasecmp(e->name, "AVG"))
        FAIL(ctx, "AVG() is not supported (jdb has no floating-point type); use SUM(x) / COUNT(x)");
      if (!e->star && !e->lhs)
        FAIL(ctx, "%s() needs an argument", e->name);
      if (e->star && strcasecmp(e->name, "COUNT") != 0)
        FAIL(ctx, "%s(*) is not valid", e->name);
      if (contains_aggregate(e->lhs))
        FAIL(ctx, "aggregate functions cannot be nested");
      return bind(ctx, e->lhs, false);
    }
    if (strcasecmp(e->name, "LENGTH") && strcasecmp(e->name, "UPPER") && strcasecmp(e->name, "LOWER") &&
        strcasecmp(e->name, "ABS") && strcasecmp(e->name, "TYPEOF"))
      FAIL(ctx, "no such function: %s", e->name);
    if (!e->lhs)
      FAIL(ctx, "%s() needs an argument", e->name);
    return bind(ctx, e->lhs, allow_aggregates);
  default:
    if (bind(ctx, e->lhs, allow_aggregates) < 0)
      return -1;
    return bind(ctx, e->rhs, allow_aggregates);
  }
}

// ---- expression evaluation ------------------------------------------------------------------

static const char *as_text(Arena *a, Value v) {
  if (v.type == VAL_TEXT)
    return v.s;
  if (v.type == VAL_INT)
    return arena_printf(a, "%lld", (long long)v.i);
  return "";
}

static int64_t as_int(Value v) {
  return v.type == VAL_INT ? v.i : v.type == VAL_TEXT ? atoll(v.s) : 0;
}

static Value copy_value(Arena *a, Value v) {
  if (v.type == VAL_TEXT)
    v.s = arena_strndup(a, v.s, (size_t)v.len);
  return v;
}

typedef struct {
  Row *rows; // the rows of one group, for aggregate functions
  int nrows;
} Group;

static Value eval(Ctx *ctx, Expr *e, Row *row, Group *g);

static Value eval_aggregate(Ctx *ctx, Expr *e, Group *g) {
  const char *f = e->name;
  if (!strcasecmp(f, "COUNT")) {
    int64_t n = 0;
    for (int i = 0; i < g->nrows; i++)
      if (e->star || eval(ctx, e->lhs, &g->rows[i], NULL).type != VAL_NULL)
        n++;
    return v_int(n);
  }
  bool any = false;
  Value best = v_null();
  int64_t sum = 0;
  for (int i = 0; i < g->nrows; i++) {
    Value v = eval(ctx, e->lhs, &g->rows[i], NULL);
    if (v.type == VAL_NULL)
      continue; // aggregates skip NULLs
    if (!strcasecmp(f, "SUM")) {
      sum += as_int(v);
    } else if (!any || (!strcasecmp(f, "MIN") ? value_compare(v, best) < 0 : value_compare(v, best) > 0)) {
      best = v;
    }
    any = true;
  }
  if (!any)
    return v_null(); // SUM/MIN/MAX of no values is NULL
  return !strcasecmp(f, "SUM") ? v_int(sum) : best;
}

static Value compare_op(const char *op, Value a, Value b) {
  if (a.type == VAL_NULL || b.type == VAL_NULL)
    return v_null(); // anything compared with NULL is unknown
  int c = value_compare(a, b);
  bool r = !strcmp(op, "=") ? c == 0 : !strcmp(op, "!=") ? c != 0 : !strcmp(op, "<") ? c < 0
         : !strcmp(op, "<=") ? c <= 0 : !strcmp(op, ">") ? c > 0 : c >= 0;
  return v_int(r);
}

static Value eval(Ctx *ctx, Expr *e, Row *row, Group *g) {
  switch (e->kind) {
  case EX_INT:
    return v_int(e->ival);
  case EX_TEXT:
    return v_text(e->sval);
  case EX_NULL:
    return v_null();
  case EX_COLUMN:
    if (!row)
      return v_null();
    return e->col == ROWID_COLUMN ? v_int(row->rowid) : row->v[e->col];
  case EX_IS_NULL: {
    bool is_null = eval(ctx, e->lhs, row, g).type == VAL_NULL;
    return v_int(e->negate ? !is_null : is_null);
  }
  case EX_FUNC: {
    if (is_aggregate_name(e->name))
      return g ? eval_aggregate(ctx, e, g) : v_null();
    Value v = eval(ctx, e->lhs, row, g);
    if (!strcasecmp(e->name, "TYPEOF"))
      return v_text(v.type == VAL_NULL ? "null" : v.type == VAL_INT ? "integer" : "text");
    if (v.type == VAL_NULL)
      return v_null();
    if (!strcasecmp(e->name, "LENGTH")) {
      // Characters, not bytes: count every UTF-8 byte except continuation bytes (10xxxxxx).
      int64_t n = 0;
      for (const unsigned char *c = (const unsigned char *)as_text(&ctx->row_arena, v); *c; c++)
        n += (*c & 0xC0) != 0x80;
      return v_int(n);
    }
    if (!strcasecmp(e->name, "ABS"))
      return v_int(llabs(as_int(v)));
    const char *s = as_text(&ctx->row_arena, v);
    char *out = arena_strndup(&ctx->row_arena, s, strlen(s));
    for (char *c = out; *c; c++)
      *c = (char)(!strcasecmp(e->name, "UPPER") ? toupper((unsigned char)*c) : tolower((unsigned char)*c));
    return v_text(out);
  }
  case EX_UNARY: {
    Value v = eval(ctx, e->lhs, row, g);
    if (v.type == VAL_NULL)
      return v_null();
    if (!strcmp(e->op, "NOT"))
      return v_int(!value_truthy(v));
    return v_int(-as_int(v));
  }
  case EX_BINARY: {
    // AND / OR use three-valued logic: TRUE, FALSE or NULL ("unknown").
    if (!strcmp(e->op, "AND") || !strcmp(e->op, "OR")) {
      bool is_and = !strcmp(e->op, "AND");
      Value a = eval(ctx, e->lhs, row, g);
      if (a.type != VAL_NULL && value_truthy(a) != is_and)
        return v_int(!is_and); // short-circuit: FALSE AND x = FALSE, TRUE OR x = TRUE
      Value b = eval(ctx, e->rhs, row, g);
      if (b.type != VAL_NULL && value_truthy(b) != is_and)
        return v_int(!is_and);
      if (a.type == VAL_NULL || b.type == VAL_NULL)
        return v_null();
      return v_int(is_and);
    }
    Value a = eval(ctx, e->lhs, row, g);
    Value b = eval(ctx, e->rhs, row, g);
    if (!strcmp(e->op, "||")) {
      if (a.type == VAL_NULL || b.type == VAL_NULL)
        return v_null();
      return v_text(arena_printf(&ctx->row_arena, "%s%s", as_text(&ctx->row_arena, a),
                                 as_text(&ctx->row_arena, b)));
    }
    if (strchr("=!<>", e->op[0]))
      return compare_op(e->op, a, b);
    if (a.type == VAL_NULL || b.type == VAL_NULL)
      return v_null();
    int64_t x = as_int(a), y = as_int(b);
    switch (e->op[0]) {
    case '+': return v_int(x + y);
    case '-': return v_int(x - y);
    case '*': return v_int(x * y);
    case '/': return y == 0 ? v_null() : v_int(x / y); // like SQLite: division by zero is NULL
    case '%': return y == 0 ? v_null() : v_int(x % y);
    }
    return v_null();
  }
  case EX_STAR:
    return v_null();
  }
  return v_null();
}

// ---- operators ------------------------------------------------------------------------------

typedef struct Op Op;
struct Op {
  bool (*next)(Op *op, Row *out);
  Ctx *ctx;
  Op *child;
};

// Scan: reads rows from the table's B+tree, optionally only keys in [lo, hi].
typedef struct {
  Op base;
  BTree tree;
  Cursor cursor;
  bool started;
  int64_t lo, hi;
} ScanOp;

static bool scan_next(Op *op, Row *out) {
  ScanOp *s = (ScanOp *)op;
  Ctx *ctx = op->ctx;
  if (!s->started) {
    btree_seek(&s->tree, s->lo, &s->cursor);
    s->started = true;
  } else {
    btree_next(&s->cursor);
  }
  if (!s->cursor.valid)
    return false;
  int64_t key = cursor_key(&s->cursor);
  if (key > s->hi)
    return false;
  arena_free(&ctx->row_arena); // the previous row has been consumed
  int len;
  const uint8_t *data = cursor_value(&s->cursor, &len);
  Table *t = ctx->table;
  out->n = t->ncols;
  out->v = arena_alloc(&ctx->row_arena, sizeof(Value) * (size_t)t->ncols);
  record_decode(&ctx->row_arena, data, len, out->v, t->ncols);
  out->rowid = key;
  if (t->pk >= 0)
    out->v[t->pk] = v_int(key); // the primary key column IS the rowid
  return true;
}

// For SELECT without FROM: exactly one empty row.
typedef struct {
  Op base;
  bool done;
} OneRowOp;

static bool onerow_next(Op *op, Row *out) {
  OneRowOp *o = (OneRowOp *)op;
  if (o->done)
    return false;
  o->done = true;
  *out = (Row){0};
  return true;
}

typedef struct {
  Op base;
  Expr *where;
} FilterOp;

static bool filter_next(Op *op, Row *out) {
  FilterOp *f = (FilterOp *)op;
  while (op->child->next(op->child, out))
    if (value_truthy(eval(op->ctx, f->where, out, NULL)))
      return true;
  return false;
}

// Project: evaluates the SELECT list (plus ORDER BY keys) for each input row.
typedef struct {
  Op base;
  Stmt *st;
  Expr **exprs; // items followed by order keys
  int nexprs;
  bool copy;    // copy into the statement arena (needed when Sort will keep rows)
} ProjectOp;

static bool project_next(Op *op, Row *out) {
  ProjectOp *p = (ProjectOp *)op;
  Row in;
  if (!op->child->next(op->child, &in))
    return false;
  Arena *a = p->copy ? op->ctx->arena : &op->ctx->row_arena;
  out->v = arena_alloc(a, sizeof(Value) * (size_t)p->nexprs);
  out->n = p->nexprs;
  out->rowid = in.rowid;
  for (int i = 0; i < p->nexprs; i++) {
    Value v = eval(op->ctx, p->exprs[i], &in, NULL);
    out->v[i] = p->copy ? copy_value(a, v) : v;
  }
  return true;
}

// Aggregate: collects all input rows, sorts them by the GROUP BY keys, and emits one row per
// group with the SELECT list (and ORDER BY keys) evaluated over that group.
typedef struct {
  Op base;
  Stmt *st;
  Expr **exprs;
  int nexprs;
  Row *rows;
  int nrows;
  int pos;
  bool loaded;
} AggOp;

static Ctx *sort_ctx;
static Expr **sort_keys;
static int sort_nkeys;

static int compare_group_rows(const Row *a, const Row *b) {
  for (int i = 0; i < sort_nkeys; i++) {
    int c = value_compare(eval(sort_ctx, sort_keys[i], (Row *)a, NULL), eval(sort_ctx, sort_keys[i], (Row *)b, NULL));
    if (c)
      return c;
  }
  return 0;
}

// A stable merge sort: rows that compare equal keep their original order.
static void merge_sort(Row *rows, int n, Row *tmp, int (*cmp)(const Row *, const Row *)) {
  if (n < 2)
    return;
  int mid = n / 2;
  merge_sort(rows, mid, tmp, cmp);
  merge_sort(rows + mid, n - mid, tmp, cmp);
  int i = 0, j = mid, k = 0;
  while (i < mid && j < n)
    tmp[k++] = cmp(&rows[j], &rows[i]) < 0 ? rows[j++] : rows[i++];
  while (i < mid)
    tmp[k++] = rows[i++];
  while (j < n)
    tmp[k++] = rows[j++];
  memcpy(rows, tmp, sizeof(Row) * (size_t)n);
}

static bool agg_next(Op *op, Row *out) {
  AggOp *a = (AggOp *)op;
  Ctx *ctx = op->ctx;
  if (!a->loaded) {
    a->loaded = true;
    int cap = 64;
    a->rows = malloc(sizeof(Row) * (size_t)cap);
    Row in;
    while (op->child->next(op->child, &in)) {
      if (a->nrows == cap) {
        cap *= 2;
        a->rows = realloc(a->rows, sizeof(Row) * (size_t)cap);
      }
      Row copy = {arena_alloc(ctx->arena, sizeof(Value) * (size_t)in.n), in.n, in.rowid};
      for (int i = 0; i < in.n; i++)
        copy.v[i] = copy_value(ctx->arena, in.v[i]);
      a->rows[a->nrows++] = copy;
    }
    if (a->st->ngroup > 0 && a->nrows > 1) {
      Row *tmp = malloc(sizeof(Row) * (size_t)a->nrows);
      sort_ctx = ctx;
      sort_keys = a->st->group_by;
      sort_nkeys = a->st->ngroup;
      merge_sort(a->rows, a->nrows, tmp, compare_group_rows);
      free(tmp);
    }
  }
  // Without GROUP BY the whole input is one group (even when empty: COUNT(*) = 0).
  if (a->st->ngroup == 0) {
    if (a->pos > 0)
      return false;
  } else if (a->pos >= a->nrows) {
    return false;
  }
  int start = a->pos, end = a->pos + 1;
  if (a->st->ngroup == 0) {
    end = a->nrows;
  } else {
    sort_ctx = ctx;
    sort_keys = a->st->group_by;
    sort_nkeys = a->st->ngroup;
    while (end < a->nrows && compare_group_rows(&a->rows[start], &a->rows[end]) == 0)
      end++;
  }
  a->pos = a->st->ngroup == 0 ? 1 : end;
  Group g = {a->rows + start, end - start};
  Row *first = g.nrows > 0 ? &g.rows[0] : NULL; // bare columns take the group's first row
  out->v = arena_alloc(ctx->arena, sizeof(Value) * (size_t)a->nexprs);
  out->n = a->nexprs;
  for (int i = 0; i < a->nexprs; i++)
    out->v[i] = copy_value(ctx->arena, eval(ctx, a->exprs[i], first, &g));
  return true;
}

// Sort: materializes all rows and orders them by the ORDER BY keys.
typedef struct {
  Op base;
  OrderItem *order;
  int norder;
  int key_start; // index of the first ORDER BY key in each row
  Row *rows;
  int nrows;
  int pos;
  bool loaded;
} SortOp;

static SortOp *active_sort;

static int compare_sort_rows(const Row *a, const Row *b) {
  for (int i = 0; i < active_sort->norder; i++) {
    int c = value_compare(a->v[active_sort->key_start + i], b->v[active_sort->key_start + i]);
    if (c)
      return active_sort->order[i].desc ? -c : c;
  }
  return 0;
}

static bool sort_next(Op *op, Row *out) {
  SortOp *s = (SortOp *)op;
  if (!s->loaded) {
    s->loaded = true;
    int cap = 64;
    s->rows = malloc(sizeof(Row) * (size_t)cap);
    Row in;
    while (op->child->next(op->child, &in)) {
      if (s->nrows == cap) {
        cap *= 2;
        s->rows = realloc(s->rows, sizeof(Row) * (size_t)cap);
      }
      s->rows[s->nrows++] = in; // already copied into the statement arena by Project/Aggregate
    }
    Row *tmp = malloc(sizeof(Row) * (size_t)(s->nrows ? s->nrows : 1));
    active_sort = s;
    merge_sort(s->rows, s->nrows, tmp, compare_sort_rows);
    free(tmp);
  }
  if (s->pos >= s->nrows)
    return false;
  *out = s->rows[s->pos++];
  return true;
}

typedef struct {
  Op base;
  int64_t limit, offset, seen, emitted;
} LimitOp;

static bool limit_next(Op *op, Row *out) {
  LimitOp *l = (LimitOp *)op;
  while (l->limit < 0 || l->emitted < l->limit) {
    if (!op->child->next(op->child, out))
      return false;
    if (l->seen++ < l->offset)
      continue;
    l->emitted++;
    return true;
  }
  return false;
}

// ---- planning ------------------------------------------------------------------------------

typedef struct {
  int64_t lo, hi;
  bool uses_key;
} KeyRange;

static bool is_rowid_ref(Ctx *ctx, Expr *e) {
  return e->kind == EX_COLUMN && (e->col == ROWID_COLUMN || (ctx->table->pk >= 0 && e->col == ctx->table->pk));
}

// Narrows [lo, hi] using every "key <op> constant" found among the top-level AND terms.
// The full WHERE clause is still checked on each row, so this can only make scans shorter,
// never change the result.
static void plan_range(Ctx *ctx, Expr *e, KeyRange *r) {
  if (!e || e->kind != EX_BINARY)
    return;
  if (!strcmp(e->op, "AND")) {
    plan_range(ctx, e->lhs, r);
    plan_range(ctx, e->rhs, r);
    return;
  }
  Expr *col = e->lhs, *val = e->rhs;
  char op[4];
  snprintf(op, sizeof op, "%s", e->op);
  if (!is_rowid_ref(ctx, col)) { // "5 < id" is "id > 5"
    col = e->rhs;
    val = e->lhs;
    if (!strcmp(op, "<")) strcpy(op, ">");
    else if (!strcmp(op, ">")) strcpy(op, "<");
    else if (!strcmp(op, "<=")) strcpy(op, ">=");
    else if (!strcmp(op, ">=")) strcpy(op, "<=");
  }
  if (!is_rowid_ref(ctx, col) || val->kind != EX_INT)
    return;
  int64_t v = val->ival;
  if (!strcmp(op, "=")) {
    if (v > r->lo) r->lo = v;
    if (v < r->hi) r->hi = v;
  } else if (!strcmp(op, ">") && v < INT64_MAX) {
    if (v + 1 > r->lo) r->lo = v + 1;
  } else if (!strcmp(op, ">=")) {
    if (v > r->lo) r->lo = v;
  } else if (!strcmp(op, "<") && v > INT64_MIN) {
    if (v - 1 < r->hi) r->hi = v - 1;
  } else if (!strcmp(op, "<=")) {
    if (v < r->hi) r->hi = v;
  } else {
    return;
  }
  r->uses_key = true;
}

static Op *make_scan(Ctx *ctx, Expr *where, KeyRange *range) {
  ScanOp *s = arena_alloc(ctx->arena, sizeof(ScanOp));
  s->base.next = scan_next;
  s->base.ctx = ctx;
  s->tree = (BTree){ctx->db->pager, ctx->table->root};
  *range = (KeyRange){INT64_MIN, INT64_MAX, false};
  plan_range(ctx, where, range);
  s->lo = range->lo;
  s->hi = range->hi;
  Op *op = &s->base;
  if (where) {
    FilterOp *f = arena_alloc(ctx->arena, sizeof(FilterOp));
    f->base = (Op){filter_next, ctx, op};
    f->where = where;
    op = &f->base;
  }
  return op;
}

// ---- output -------------------------------------------------------------------------------

static void expr_text(Arena *a, Expr *e, char *buf, size_t cap) {
  switch (e->kind) {
  case EX_COLUMN: snprintf(buf, cap, "%s", e->name); return;
  case EX_INT: snprintf(buf, cap, "%lld", (long long)e->ival); return;
  case EX_TEXT: snprintf(buf, cap, "'%s'", e->sval); return;
  case EX_NULL: snprintf(buf, cap, "NULL"); return;
  case EX_STAR: snprintf(buf, cap, "*"); return;
  case EX_FUNC: {
    char inner[128] = "*";
    if (!e->star && e->lhs)
      expr_text(a, e->lhs, inner, sizeof inner);
    snprintf(buf, cap, "%s(%s)", e->name, inner);
    return;
  }
  case EX_IS_NULL: {
    char l[128];
    expr_text(a, e->lhs, l, sizeof l);
    snprintf(buf, cap, "%s IS %sNULL", l, e->negate ? "NOT " : "");
    return;
  }
  case EX_UNARY: {
    char l[128];
    expr_text(a, e->lhs, l, sizeof l);
    snprintf(buf, cap, "%s%s%s", e->op, strcmp(e->op, "NOT") ? "" : " ", l);
    return;
  }
  case EX_BINARY: {
    char l[128], r[128];
    expr_text(a, e->lhs, l, sizeof l);
    expr_text(a, e->rhs, r, sizeof r);
    snprintf(buf, cap, "%s %s %s", l, e->op, r);
    return;
  }
  }
}

typedef struct {
  OutputOptions *opt;
  Arena *arena;
  int ncols;
  char **headers;
  // table mode buffers everything to compute column widths
  char ***cells;
  int nrows, cap;
} Printer;

static void print_row(Printer *p, Row *row) {
  if (p->opt->mode == OUT_LIST) {
    for (int i = 0; i < p->ncols; i++) {
      if (i)
        fputc('|', p->opt->out);
      if (row->v[i].type != VAL_NULL)
        fputs(as_text(p->arena, row->v[i]), p->opt->out);
    }
    fputc('\n', p->opt->out);
    return;
  }
  if (p->nrows == p->cap) {
    p->cap = p->cap ? p->cap * 2 : 64;
    p->cells = realloc(p->cells, sizeof(char **) * (size_t)p->cap);
  }
  char **cells = arena_alloc(p->arena, sizeof(char *) * (size_t)p->ncols);
  for (int i = 0; i < p->ncols; i++)
    cells[i] = row->v[i].type == VAL_NULL ? "NULL" : arena_strndup(p->arena, as_text(p->arena, row->v[i]),
                                                                   strlen(as_text(p->arena, row->v[i])));
  p->cells[p->nrows++] = cells;
}

static void print_rule(FILE *out, int *w, int n, const char *l, const char *m, const char *r) {
  fputs(l, out);
  for (int i = 0; i < n; i++) {
    for (int k = 0; k < w[i] + 2; k++)
      fputs("─", out);
    fputs(i + 1 < n ? m : r, out);
  }
  fputc('\n', out);
}

static void print_finish(Printer *p) {
  FILE *out = p->opt->out;
  if (p->opt->mode == OUT_LIST) {
    return;
  }
  int *w = calloc((size_t)p->ncols, sizeof(int));
  for (int i = 0; i < p->ncols; i++) {
    w[i] = (int)strlen(p->headers[i]);
    for (int r = 0; r < p->nrows; r++)
      if ((int)strlen(p->cells[r][i]) > w[i])
        w[i] = (int)strlen(p->cells[r][i]);
  }
  print_rule(out, w, p->ncols, "┌", "┬", "┐");
  fputs("│", out);
  for (int i = 0; i < p->ncols; i++)
    fprintf(out, " %-*s │", w[i], p->headers[i]);
  fputc('\n', out);
  print_rule(out, w, p->ncols, "├", "┼", "┤");
  for (int r = 0; r < p->nrows; r++) {
    fputs("│", out);
    for (int i = 0; i < p->ncols; i++) {
      const char *c = p->cells[r][i];
      bool numeric = *c && (isdigit((unsigned char)*c) || (*c == '-' && isdigit((unsigned char)c[1])));
      if (numeric)
        fprintf(out, " %*s │", w[i], c); // numbers right-aligned
      else
        fprintf(out, " %-*s │", w[i], c);
    }
    fputc('\n', out);
  }
  print_rule(out, w, p->ncols, "└", "┴", "┘");
  fprintf(out, "(%d row%s)\n", p->nrows, p->nrows == 1 ? "" : "s");
  free(w);
  free(p->cells);
}

// ---- statements ----------------------------------------------------------------------------

static int exec_select(Ctx *ctx, Stmt *st, OutputOptions *opt, bool explain) {
  if (st->table) {
    ctx->table = db_find_table(ctx->db, st->table);
    if (!ctx->table)
      FAIL(ctx, "no such table: %s", st->table);
  }

  // Expand * into every column, then resolve names.
  int nitems = 0;
  SelectItem *items = arena_alloc(ctx->arena, sizeof(SelectItem) * (MAX_COLUMNS * 2 + 1));
  for (int i = 0; i < st->nitems; i++) {
    if (st->items[i].expr->kind == EX_STAR) {
      if (!ctx->table)
        FAIL(ctx, "no tables specified");
      for (int c = 0; c < ctx->table->ncols && nitems < MAX_COLUMNS * 2; c++) {
        Expr *e = arena_alloc(ctx->arena, sizeof(Expr));
        e->kind = EX_COLUMN;
        e->name = ctx->table->cols[c].name;
        items[nitems++] = (SelectItem){e, NULL};
      }
    } else if (nitems < MAX_COLUMNS * 2) {
      items[nitems++] = st->items[i];
    }
  }

  bool aggregate = st->ngroup > 0;
  for (int i = 0; i < nitems; i++)
    aggregate |= contains_aggregate(items[i].expr);
  for (int i = 0; i < st->norder; i++)
    aggregate |= contains_aggregate(st->order_by[i].expr);

  for (int i = 0; i < nitems; i++)
    if (bind(ctx, items[i].expr, aggregate) < 0)
      return -1;
  if (bind(ctx, st->where, false) < 0)
    return -1;
  for (int i = 0; i < st->ngroup; i++)
    if (bind(ctx, st->group_by[i], false) < 0)
      return -1;

  // ORDER BY may name a result column by alias or position (ORDER BY 2).
  Expr **exprs = arena_alloc(ctx->arena, sizeof(Expr *) * (size_t)(nitems + st->norder + 1));
  for (int i = 0; i < nitems; i++)
    exprs[i] = items[i].expr;
  for (int i = 0; i < st->norder; i++) {
    Expr *o = st->order_by[i].expr;
    if (o->kind == EX_INT && o->ival >= 1 && o->ival <= nitems) {
      o = items[o->ival - 1].expr;
    } else if (o->kind == EX_COLUMN) {
      for (int k = 0; k < nitems; k++)
        if (items[k].alias && !strcasecmp(items[k].alias, o->name))
          o = items[k].expr;
    }
    if (o->col == -1 && bind(ctx, o, aggregate) < 0)
      return -1;
    exprs[nitems + i] = o;
  }
  int nexprs = nitems + st->norder;

  // Build the pipeline bottom-up.
  KeyRange range = {INT64_MIN, INT64_MAX, false};
  Op *op;
  if (ctx->table) {
    op = make_scan(ctx, st->where, &range);
  } else {
    OneRowOp *o = arena_alloc(ctx->arena, sizeof(OneRowOp));
    o->base = (Op){onerow_next, ctx, NULL};
    op = &o->base;
    if (st->where) {
      FilterOp *f = arena_alloc(ctx->arena, sizeof(FilterOp));
      f->base = (Op){filter_next, ctx, op};
      f->where = st->where;
      op = &f->base;
    }
  }
  if (aggregate) {
    AggOp *a = arena_alloc(ctx->arena, sizeof(AggOp));
    a->base = (Op){agg_next, ctx, op};
    a->st = st;
    a->exprs = exprs;
    a->nexprs = nexprs;
    op = &a->base;
  } else {
    ProjectOp *p = arena_alloc(ctx->arena, sizeof(ProjectOp));
    p->base = (Op){project_next, ctx, op};
    p->st = st;
    p->exprs = exprs;
    p->nexprs = nexprs;
    p->copy = st->norder > 0;
    op = &p->base;
  }
  if (st->norder > 0) {
    SortOp *s = arena_alloc(ctx->arena, sizeof(SortOp));
    s->base = (Op){sort_next, ctx, op};
    s->order = st->order_by;
    s->norder = st->norder;
    s->key_start = nitems;
    op = &s->base;
  }
  if (st->limit >= 0 || st->offset > 0) {
    LimitOp *l = arena_alloc(ctx->arena, sizeof(LimitOp));
    l->base = (Op){limit_next, ctx, op};
    l->limit = st->limit;
    l->offset = st->offset;
    op = &l->base;
  }

  if (explain) {
    FILE *out = opt->out;
    if (!ctx->table)
      fprintf(out, "CONSTANT ROW\n");
    else if (range.uses_key)
      fprintf(out, "SEARCH %s USING INTEGER PRIMARY KEY (rowid %lld..%lld)\n", ctx->table->name,
              (long long)range.lo, (long long)range.hi);
    else
      fprintf(out, "SCAN %s (full table)\n", ctx->table->name);
    if (st->where)
      fprintf(out, "FILTER\n");
    if (aggregate)
      fprintf(out, "AGGREGATE%s\n", st->ngroup ? " (sort by GROUP BY keys)" : "");
    if (st->norder)
      fprintf(out, "SORT for ORDER BY\n");
    if (st->limit >= 0 || st->offset)
      fprintf(out, "LIMIT\n");
    return 0;
  }

  Printer pr = {opt, ctx->arena, nitems, NULL, NULL, 0, 0};
  pr.headers = arena_alloc(ctx->arena, sizeof(char *) * (size_t)nitems);
  for (int i = 0; i < nitems; i++) {
    if (items[i].alias) {
      pr.headers[i] = items[i].alias;
    } else {
      char buf[256];
      expr_text(ctx->arena, items[i].expr, buf, sizeof buf);
      pr.headers[i] = arena_strndup(ctx->arena, buf, strlen(buf));
    }
  }
  if (opt->mode == OUT_LIST && opt->headers) {
    for (int i = 0; i < nitems; i++)
      fprintf(opt->out, "%s%s", i ? "|" : "", pr.headers[i]);
    fputc('\n', opt->out);
  }
  Row row;
  while (op->next(op, &row))
    print_row(&pr, &row);
  print_finish(&pr);
  // free the buffers held by Sort and Aggregate operators
  for (Op *o = op; o; o = o->child) {
    if (o->next == sort_next)
      free(((SortOp *)o)->rows);
    if (o->next == agg_next)
      free(((AggOp *)o)->rows);
  }
  return 0;
}

// Converts a value to the column's type, like SQLite's "type affinity":
// '42' stored into an INTEGER column becomes 42; 42 stored into a TEXT column becomes '42'.
static Value apply_affinity(Ctx *ctx, Column *c, Value v) {
  if (v.type == VAL_NULL)
    return v;
  if (c->type == COL_INTEGER && v.type == VAL_TEXT) {
    char *end;
    long long x = strtoll(v.s, &end, 10);
    if (*v.s && *end == '\0')
      return v_int(x);
    return v;
  }
  if (c->type == COL_TEXT && v.type == VAL_INT)
    return v_text(arena_printf(ctx->arena, "%lld", (long long)v.i));
  return v;
}

static int check_not_null(Ctx *ctx, Table *t, Value *vals) {
  for (int c = 0; c < t->ncols; c++)
    if (t->cols[c].not_null && c != t->pk && vals[c].type == VAL_NULL)
      FAIL(ctx, "NOT NULL constraint failed: %s.%s", t->name, t->cols[c].name);
  return 0;
}

static int encode_row(Ctx *ctx, Table *t, Value *vals, uint8_t *buf) {
  Value stored[MAX_COLUMNS];
  memcpy(stored, vals, sizeof(Value) * (size_t)t->ncols);
  if (t->pk >= 0)
    stored[t->pk] = v_null(); // the key is stored once, as the B+tree key
  int len = record_encode(stored, t->ncols, buf, MAX_ROW_SIZE);
  if (len < 0)
    FAIL(ctx, "row too large (the limit is %d bytes)", MAX_ROW_SIZE);
  return len;
}

typedef struct {
  int64_t rowid;
  Value *vals;
} PendingRow;

// INSERT checks every row (types, constraints, duplicate keys) BEFORE changing anything, so a
// failing multi-row INSERT leaves the table exactly as it was.
static int exec_insert(Ctx *ctx, Stmt *st, int64_t *changes) {
  Table *t = db_find_table(ctx->db, st->table);
  if (!t)
    FAIL(ctx, "no such table: %s", st->table);
  ctx->table = t;
  int map[MAX_COLUMNS]; // value position -> column index
  int nmap = st->n_insert_cols ? st->n_insert_cols : t->ncols;
  for (int i = 0; i < nmap; i++) {
    if (st->n_insert_cols) {
      map[i] = find_column(t, st->insert_cols[i]);
      if (map[i] < 0)
        FAIL(ctx, "table %s has no column named %s", t->name, st->insert_cols[i]);
    } else {
      map[i] = i;
    }
  }
  BTree tree = {ctx->db->pager, t->root};
  int64_t next_rowid = btree_max_key(&tree) + 1;
  PendingRow *rows = arena_alloc(ctx->arena, sizeof(PendingRow) * (size_t)st->nrows);
  for (int r = 0; r < st->nrows; r++) {
    int n = 0;
    while (st->rows[r][n])
      n++;
    if (n != nmap)
      FAIL(ctx, "%d values for %d columns", n, nmap);
    Value *vals = arena_alloc(ctx->arena, sizeof(Value) * MAX_COLUMNS);
    for (int c = 0; c < t->ncols; c++)
      vals[c] = v_null();
    for (int i = 0; i < n; i++) {
      if (bind(ctx, st->rows[r][i], false) < 0)
        return -1;
      if (map[i] == ROWID_COLUMN)
        FAIL(ctx, "cannot set rowid directly; declare an INTEGER PRIMARY KEY column");
      vals[map[i]] = copy_value(ctx->arena, apply_affinity(ctx, &t->cols[map[i]], eval(ctx, st->rows[r][i], NULL, NULL)));
    }
    int64_t rowid;
    if (t->pk >= 0 && vals[t->pk].type != VAL_NULL) {
      if (vals[t->pk].type != VAL_INT)
        FAIL(ctx, "datatype mismatch: %s must be an integer", t->cols[t->pk].name);
      rowid = vals[t->pk].i;
    } else {
      rowid = next_rowid;
    }
    if (rowid >= next_rowid)
      next_rowid = rowid + 1;
    if (check_not_null(ctx, t, vals) < 0)
      return -1;
    bool dup = btree_find(&tree, rowid, NULL, NULL);
    for (int k = 0; k < r && !dup; k++)
      dup = rows[k].rowid == rowid;
    if (dup)
      FAIL(ctx, "UNIQUE constraint failed: %s.%s", t->name, t->pk >= 0 ? t->cols[t->pk].name : "rowid");
    uint8_t buf[MAX_ROW_SIZE];
    if (encode_row(ctx, t, vals, buf) < 0)
      return -1;
    rows[r] = (PendingRow){rowid, vals};
  }
  for (int r = 0; r < st->nrows; r++) {
    uint8_t buf[MAX_ROW_SIZE];
    int len = encode_row(ctx, t, rows[r].vals, buf);
    btree_insert(&tree, rows[r].rowid, buf, len);
  }
  *changes = st->nrows;
  return 0;
}

// UPDATE and DELETE first COLLECT the matching rows, then modify them. Changing the B+tree while
// a cursor is walking it could skip or revisit rows (a variant of the "Halloween problem").
static int collect_matches(Ctx *ctx, Expr *where, PendingRow **out, int *n) {
  if (bind(ctx, where, false) < 0)
    return -1;
  KeyRange range;
  Op *op = make_scan(ctx, where, &range);
  int cap = 64;
  *out = malloc(sizeof(PendingRow) * (size_t)cap);
  *n = 0;
  Row row;
  while (op->next(op, &row)) {
    if (*n == cap) {
      cap *= 2;
      *out = realloc(*out, sizeof(PendingRow) * (size_t)cap);
    }
    Value *vals = arena_alloc(ctx->arena, sizeof(Value) * (size_t)row.n);
    for (int i = 0; i < row.n; i++)
      vals[i] = copy_value(ctx->arena, row.v[i]);
    (*out)[(*n)++] = (PendingRow){row.rowid, vals};
  }
  return 0;
}

static int exec_update(Ctx *ctx, Stmt *st, int64_t *changes) {
  Table *t = db_find_table(ctx->db, st->table);
  if (!t)
    FAIL(ctx, "no such table: %s", st->table);
  ctx->table = t;
  int cols[MAX_COLUMNS];
  for (int i = 0; i < st->nset; i++) {
    cols[i] = find_column(t, st->set_cols[i]);
    if (cols[i] < 0)
      FAIL(ctx, "no such column: %s", st->set_cols[i]);
    if (cols[i] == t->pk || cols[i] == ROWID_COLUMN)
      FAIL(ctx, "jdb does not support changing the primary key with UPDATE");
    if (bind(ctx, st->set_vals[i], false) < 0)
      return -1;
  }
  PendingRow *rows;
  int n;
  if (collect_matches(ctx, st->where, &rows, &n) < 0)
    return -1;
  // Compute every new row first (all SET expressions see the OLD values), then write.
  uint8_t (*bufs)[MAX_ROW_SIZE] = malloc(sizeof(*bufs) * (size_t)(n ? n : 1));
  int *lens = malloc(sizeof(int) * (size_t)(n ? n : 1));
  int rc = 0;
  for (int r = 0; r < n && rc == 0; r++) {
    Row old = {rows[r].vals, t->ncols, rows[r].rowid};
    Value newv[MAX_COLUMNS];
    memcpy(newv, rows[r].vals, sizeof(Value) * (size_t)t->ncols);
    for (int i = 0; i < st->nset; i++)
      newv[cols[i]] = copy_value(ctx->arena, apply_affinity(ctx, &t->cols[cols[i]], eval(ctx, st->set_vals[i], &old, NULL)));
    if (check_not_null(ctx, t, newv) < 0 || (lens[r] = encode_row(ctx, t, newv, bufs[r])) < 0)
      rc = -1;
  }
  if (rc == 0) {
    BTree tree = {ctx->db->pager, t->root};
    for (int r = 0; r < n; r++) {
      btree_delete(&tree, rows[r].rowid);
      btree_insert(&tree, rows[r].rowid, bufs[r], lens[r]);
    }
    *changes = n;
  }
  free(bufs);
  free(lens);
  free(rows);
  return rc;
}

static int exec_delete(Ctx *ctx, Stmt *st, int64_t *changes) {
  Table *t = db_find_table(ctx->db, st->table);
  if (!t)
    FAIL(ctx, "no such table: %s", st->table);
  ctx->table = t;
  PendingRow *rows;
  int n;
  if (collect_matches(ctx, st->where, &rows, &n) < 0)
    return -1;
  BTree tree = {ctx->db->pager, t->root};
  for (int r = 0; r < n; r++)
    btree_delete(&tree, rows[r].rowid);
  free(rows);
  *changes = n;
  return 0;
}

static int exec_create(Ctx *ctx, Stmt *st) {
  if (db_find_table(ctx->db, st->table)) {
    if (st->if_not_exists)
      return 0;
    FAIL(ctx, "table %s already exists", st->table);
  }
  if (ctx->db->ntables == MAX_TABLES)
    FAIL(ctx, "too many tables (the limit is %d)", MAX_TABLES);
  Table *t = calloc(1, sizeof(Table));
  snprintf(t->name, sizeof t->name, "%s", st->table);
  t->ncols = st->ncols;
  memcpy(t->cols, st->cols, sizeof(Column) * (size_t)st->ncols);
  t->pk = -1;
  for (int i = 0; i < t->ncols; i++)
    if (t->cols[i].primary_key)
      t->pk = i;
  t->root = btree_create(ctx->db->pager);
  t->sql = strdup(st->sql_text);
  ctx->db->tables[ctx->db->ntables++] = t;
  catalog_save(ctx->db);
  return 0;
}

static int exec_drop(Ctx *ctx, Stmt *st) {
  Database *db = ctx->db;
  for (int i = 0; i < db->ntables; i++) {
    Table *t = db->tables[i];
    if (strcasecmp(t->name, st->table) != 0)
      continue;
    BTree tree = {db->pager, t->root};
    btree_free_all(&tree);
    free(t->sql);
    free(t);
    memmove(db->tables + i, db->tables + i + 1, sizeof(Table *) * (size_t)(db->ntables - i - 1));
    db->ntables--;
    catalog_save(db);
    return 0;
  }
  if (st->if_not_exists)
    return 0;
  FAIL(ctx, "no such table: %s", st->table);
}

// After a rollback the on-disk catalog is the truth again: reload the in-memory table list.
static void reload_catalog(Database *db) {
  for (int i = 0; i < db->ntables; i++) {
    free(db->tables[i]->sql);
    free(db->tables[i]);
  }
  db->ntables = 0;
  catalog_load(db);
}

int execute(Database *db, Arena *a, Stmt *st, OutputOptions *opt, char **err) {
  Ctx ctx = {.db = db, .arena = a};
  Pager *pager = db->pager;
  int rc = 0;
  int64_t changes = -1;
  switch (st->kind) {
  case ST_BEGIN:
    if (pager->in_txn) {
      *err = "cannot start a transaction within a transaction";
      return -1;
    }
    pager_begin(pager);
    return 0;
  case ST_COMMIT:
    if (!pager->in_txn) {
      *err = "cannot commit - no transaction is active";
      return -1;
    }
    pager_commit(pager);
    return 0;
  case ST_ROLLBACK:
    if (!pager->in_txn) {
      *err = "cannot rollback - no transaction is active";
      return -1;
    }
    pager_rollback(pager);
    reload_catalog(db);
    return 0;
  case ST_EXPLAIN:
    if (st->inner->kind != ST_SELECT) {
      *err = "EXPLAIN is only supported for SELECT";
      return -1;
    }
    rc = exec_select(&ctx, st->inner, opt, true);
    break;
  case ST_SELECT:
    rc = exec_select(&ctx, st, opt, false);
    break;
  case ST_CREATE: rc = exec_create(&ctx, st); break;
  case ST_DROP: rc = exec_drop(&ctx, st); break;
  case ST_INSERT: rc = exec_insert(&ctx, st, &changes); break;
  case ST_UPDATE: rc = exec_update(&ctx, st, &changes); break;
  case ST_DELETE: rc = exec_delete(&ctx, st, &changes); break;
  }
  arena_free(&ctx.row_arena);
  if (rc < 0) {
    *err = ctx.err;
    // Statements validate before writing, so a failed statement changed nothing.
    return -1;
  }
  if (changes >= 0 && opt->mode == OUT_TABLE)
    fprintf(opt->out, "(%lld row%s changed)\n", (long long)changes, changes == 1 ? "" : "s");
  if (!pager->in_txn)
    pager_commit(pager); // autocommit: every statement outside BEGIN is its own transaction
  return 0;
}

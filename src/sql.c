// SQL front end: a tokenizer and a recursive-descent parser that builds a Stmt tree.
//
// Supported statements:
//   CREATE TABLE [IF NOT EXISTS] t (col INTEGER PRIMARY KEY, col TEXT NOT NULL, ...)
//   DROP TABLE [IF EXISTS] t
//   INSERT INTO t [(cols)] VALUES (...), (...)
//   SELECT * | expr [AS name], ... [FROM t] [WHERE e] [GROUP BY e, ...]
//          [ORDER BY e [ASC|DESC], ...] [LIMIT n [OFFSET m]]
//   UPDATE t SET col = e, ... [WHERE e]
//   DELETE FROM t [WHERE e]
//   BEGIN | COMMIT | ROLLBACK | EXPLAIN <statement>
//
// Expression precedence (lowest first), as in SQLite:
//   OR | AND | NOT | = == != <> < <= > >= IS [NOT] NULL BETWEEN | + - | * / % | || | unary - | primary
//
// Errors use setjmp/longjmp: on a syntax error the parser jumps straight back to
// parse_statement(), which reports it. All memory comes from the statement's arena, so nothing
// leaks when parsing is abandoned halfway.

#include "jdb.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef enum { T_IDENT, T_NUM, T_STR, T_PUNCT, T_EOF } TokKind;

typedef struct {
  TokKind kind;
  const char *start;
  int len;
  int64_t num;
  char *str;
} Tok;

typedef struct {
  Arena *arena;
  const char *src;
  const char *pos;
  Tok tok;        // current token
  jmp_buf on_error;
  char *err;
} Parser;

__attribute__((noreturn)) static void fail(Parser *p, const char *msg) {
  if (p->tok.kind == T_EOF)
    p->err = arena_printf(p->arena, "incomplete input: %s", msg);
  else
    p->err = arena_printf(p->arena, "near \"%.*s\": %s", p->tok.len, p->tok.start, msg);
  longjmp(p->on_error, 1);
}

static void next(Parser *p) {
  const char *s = p->pos;
  for (;;) {
    while (isspace((unsigned char)*s))
      s++;
    if (s[0] == '-' && s[1] == '-') { // -- comment to end of line
      while (*s && *s != '\n')
        s++;
      continue;
    }
    break;
  }
  Tok t = {.start = s};
  if (*s == '\0') {
    t.kind = T_EOF;
  } else if (isdigit((unsigned char)*s)) {
    t.kind = T_NUM;
    char *end;
    t.num = strtoll(s, &end, 10);
    s = end;
  } else if (isalpha((unsigned char)*s) || *s == '_') {
    t.kind = T_IDENT;
    while (isalnum((unsigned char)*s) || *s == '_')
      s++;
  } else if (*s == '"') { // "quoted identifier"
    const char *q = ++s;
    while (*s && *s != '"')
      s++;
    if (!*s) {
      p->tok = t;
      fail(p, "unterminated quoted identifier");
    }
    t.kind = T_IDENT;
    t.start = q;
    t.len = (int)(s - q);
    p->tok = t;
    p->pos = s + 1;
    return;
  } else if (*s == '\'') { // 'string literal', with '' as an escaped quote
    s++;
    char *buf = arena_alloc(p->arena, strlen(s) + 1);
    int n = 0;
    for (;;) {
      if (*s == '\0') {
        p->tok = t;
        fail(p, "unterminated string literal");
      }
      if (*s == '\'' && s[1] == '\'') {
        buf[n++] = '\'';
        s += 2;
      } else if (*s == '\'') {
        s++;
        break;
      } else {
        buf[n++] = *s++;
      }
    }
    t.kind = T_STR;
    t.str = buf;
  } else {
    t.kind = T_PUNCT;
    static const char *two[] = {"<=", ">=", "!=", "<>", "==", "||"};
    int len = 1;
    for (size_t i = 0; i < sizeof two / sizeof *two; i++)
      if (strncmp(s, two[i], 2) == 0)
        len = 2;
    if (len == 1 && !strchr("(),;*+-/%=<>.", *s)) {
      t.len = 1;
      p->tok = t;
      fail(p, "unrecognized character");
    }
    s += len;
  }
  if (t.kind != T_IDENT || t.len == 0)
    t.len = (int)(s - t.start);
  p->tok = t;
  p->pos = s;
}

static bool is(Parser *p, const char *word) {
  return (p->tok.kind == T_IDENT || p->tok.kind == T_PUNCT) && (int)strlen(word) == p->tok.len &&
         strncasecmp(p->tok.start, word, (size_t)p->tok.len) == 0;
}

static bool accept(Parser *p, const char *word) {
  if (is(p, word)) {
    next(p);
    return true;
  }
  return false;
}

static void expect(Parser *p, const char *word) {
  if (!accept(p, word))
    fail(p, arena_printf(p->arena, "syntax error, expected %s", word));
}

static bool is_reserved(Parser *p) {
  static const char *kw[] = {
    "SELECT", "FROM", "WHERE", "GROUP", "ORDER", "BY", "LIMIT", "OFFSET", "INSERT", "INTO",
    "VALUES", "UPDATE", "SET", "DELETE", "CREATE", "DROP", "TABLE", "AND", "OR", "NOT", "NULL",
    "IS", "AS", "ASC", "DESC", "BETWEEN",
  };
  for (size_t i = 0; i < sizeof kw / sizeof *kw; i++)
    if (is(p, kw[i]))
      return true;
  return false;
}

static char *ident(Parser *p) {
  if (p->tok.kind != T_IDENT || is_reserved(p))
    fail(p, "syntax error, expected a name");
  char *s = arena_strndup(p->arena, p->tok.start, (size_t)p->tok.len);
  next(p);
  return s;
}

// ---- expressions ------------------------------------------------------------------------

static Expr *new_expr(Parser *p, ExprKind kind) {
  Expr *e = arena_alloc(p->arena, sizeof(Expr));
  e->kind = kind;
  e->col = -1;
  return e;
}

static Expr *binary(Parser *p, const char *op, Expr *l, Expr *r) {
  Expr *e = new_expr(p, EX_BINARY);
  snprintf(e->op, sizeof e->op, "%s", op);
  e->lhs = l;
  e->rhs = r;
  return e;
}

static Expr *expr(Parser *p);

static Expr *primary(Parser *p) {
  Tok t = p->tok;
  if (t.kind == T_NUM) {
    next(p);
    Expr *e = new_expr(p, EX_INT);
    e->ival = t.num;
    return e;
  }
  if (t.kind == T_STR) {
    next(p);
    Expr *e = new_expr(p, EX_TEXT);
    e->sval = t.str;
    return e;
  }
  if (accept(p, "NULL"))
    return new_expr(p, EX_NULL);
  if (accept(p, "(")) {
    Expr *e = expr(p);
    expect(p, ")");
    return e;
  }
  if (t.kind == T_IDENT && !is_reserved(p)) {
    char *name = ident(p);
    if (accept(p, "(")) { // function call: COUNT(*), SUM(x), LENGTH(s), ...
      Expr *e = new_expr(p, EX_FUNC);
      e->name = name;
      if (accept(p, "*")) {
        e->star = true;
      } else if (!is(p, ")")) {
        e->lhs = expr(p);
      }
      expect(p, ")");
      return e;
    }
    Expr *e = new_expr(p, EX_COLUMN);
    e->name = name;
    return e;
  }
  fail(p, "syntax error");
}

static Expr *unary(Parser *p) {
  if (accept(p, "-")) {
    Expr *e = new_expr(p, EX_UNARY);
    strcpy(e->op, "-");
    e->lhs = unary(p);
    return e;
  }
  if (accept(p, "+"))
    return unary(p);
  return primary(p);
}

static Expr *concat(Parser *p) {
  Expr *e = unary(p);
  while (accept(p, "||"))
    e = binary(p, "||", e, unary(p));
  return e;
}

static Expr *mul(Parser *p) {
  Expr *e = concat(p);
  for (;;) {
    if (accept(p, "*"))
      e = binary(p, "*", e, concat(p));
    else if (accept(p, "/"))
      e = binary(p, "/", e, concat(p));
    else if (accept(p, "%"))
      e = binary(p, "%", e, concat(p));
    else
      return e;
  }
}

static Expr *add(Parser *p) {
  Expr *e = mul(p);
  for (;;) {
    if (accept(p, "+"))
      e = binary(p, "+", e, mul(p));
    else if (accept(p, "-"))
      e = binary(p, "-", e, mul(p));
    else
      return e;
  }
}

static Expr *comparison(Parser *p) {
  Expr *e = add(p);
  for (;;) {
    if (accept(p, "=") || accept(p, "=="))
      e = binary(p, "=", e, add(p));
    else if (accept(p, "!=") || accept(p, "<>"))
      e = binary(p, "!=", e, add(p));
    else if (accept(p, "<="))
      e = binary(p, "<=", e, add(p));
    else if (accept(p, ">="))
      e = binary(p, ">=", e, add(p));
    else if (accept(p, "<"))
      e = binary(p, "<", e, add(p));
    else if (accept(p, ">"))
      e = binary(p, ">", e, add(p));
    else if (accept(p, "IS")) {
      Expr *n = new_expr(p, EX_IS_NULL);
      n->negate = accept(p, "NOT");
      expect(p, "NULL");
      n->lhs = e;
      e = n;
    } else if (accept(p, "BETWEEN")) {
      // a BETWEEN x AND y  ==  a >= x AND a <= y
      Expr *lo = add(p);
      expect(p, "AND");
      Expr *hi = add(p);
      e = binary(p, "AND", binary(p, ">=", e, lo), binary(p, "<=", e, hi));
    } else {
      return e;
    }
  }
}

static Expr *not_expr(Parser *p) {
  if (accept(p, "NOT")) {
    Expr *e = new_expr(p, EX_UNARY);
    strcpy(e->op, "NOT");
    e->lhs = not_expr(p);
    return e;
  }
  return comparison(p);
}

static Expr *and_expr(Parser *p) {
  Expr *e = not_expr(p);
  while (accept(p, "AND"))
    e = binary(p, "AND", e, not_expr(p));
  return e;
}

static Expr *expr(Parser *p) {
  Expr *e = and_expr(p);
  while (accept(p, "OR"))
    e = binary(p, "OR", e, and_expr(p));
  return e;
}

// ---- statements ---------------------------------------------------------------------------

static void parse_create(Parser *p, Stmt *st, const char *stmt_start) {
  st->kind = ST_CREATE;
  expect(p, "TABLE");
  if (accept(p, "IF")) {
    expect(p, "NOT");
    expect(p, "EXISTS");
    st->if_not_exists = true;
  }
  st->table = ident(p);
  expect(p, "(");
  do {
    if (st->ncols == MAX_COLUMNS)
      fail(p, "too many columns");
    Column *c = &st->cols[st->ncols++];
    snprintf(c->name, sizeof c->name, "%s", ident(p));
    if (accept(p, "INTEGER") || accept(p, "INT") || accept(p, "BIGINT")) {
      c->type = COL_INTEGER;
    } else if (accept(p, "TEXT") || accept(p, "VARCHAR") || accept(p, "CHAR")) {
      c->type = COL_TEXT;
      if (accept(p, "(")) { // VARCHAR(n): the length is accepted and ignored, as in SQLite
        if (p->tok.kind != T_NUM)
          fail(p, "expected a length");
        next(p);
        expect(p, ")");
      }
    } else {
      fail(p, "expected a column type: INTEGER or TEXT");
    }
    for (;;) {
      if (accept(p, "PRIMARY")) {
        expect(p, "KEY");
        if (c->type != COL_INTEGER)
          fail(p, "jdb only supports INTEGER PRIMARY KEY");
        for (int i = 0; i < st->ncols - 1; i++)
          if (st->cols[i].primary_key)
            fail(p, "a table can have only one primary key");
        c->primary_key = true;
      } else if (accept(p, "NOT")) {
        expect(p, "NULL");
        c->not_null = true;
      } else {
        break;
      }
    }
    for (int i = 0; i < st->ncols - 1; i++)
      if (strcasecmp(st->cols[i].name, c->name) == 0)
        fail(p, arena_printf(p->arena, "duplicate column name: %s", c->name));
  } while (accept(p, ","));
  expect(p, ")");
  st->sql_text = arena_strndup(p->arena, stmt_start, (size_t)(p->tok.start - stmt_start));
  // trim trailing whitespace from the stored text
  for (size_t n = strlen(st->sql_text); n > 0 && isspace((unsigned char)st->sql_text[n - 1]); n--)
    st->sql_text[n - 1] = '\0';
}

static void parse_insert(Parser *p, Stmt *st) {
  st->kind = ST_INSERT;
  expect(p, "INTO");
  st->table = ident(p);
  if (accept(p, "(")) {
    do {
      if (st->n_insert_cols == MAX_COLUMNS)
        fail(p, "too many columns");
      st->insert_cols[st->n_insert_cols++] = ident(p);
    } while (accept(p, ","));
    expect(p, ")");
  }
  expect(p, "VALUES");
  int cap = 8;
  st->rows = arena_alloc(p->arena, sizeof(Expr **) * (size_t)cap);
  do {
    expect(p, "(");
    Expr **row = arena_alloc(p->arena, sizeof(Expr *) * (MAX_COLUMNS + 1));
    int n = 0;
    do {
      if (n == MAX_COLUMNS)
        fail(p, "too many values");
      row[n++] = expr(p);
    } while (accept(p, ","));
    row[n] = NULL;
    expect(p, ")");
    if (st->nrows == cap) {
      Expr ***bigger = arena_alloc(p->arena, sizeof(Expr **) * (size_t)cap * 2);
      memcpy(bigger, st->rows, sizeof(Expr **) * (size_t)cap);
      st->rows = bigger;
      cap *= 2;
    }
    st->rows[st->nrows++] = row;
  } while (accept(p, ","));
}

static Expr **expr_list(Parser *p, int *n) {
  Expr **list = arena_alloc(p->arena, sizeof(Expr *) * MAX_COLUMNS);
  *n = 0;
  do {
    if (*n == MAX_COLUMNS)
      fail(p, "too many expressions");
    list[(*n)++] = expr(p);
  } while (accept(p, ","));
  return list;
}

static void parse_select(Parser *p, Stmt *st) {
  st->kind = ST_SELECT;
  st->limit = -1;
  st->items = arena_alloc(p->arena, sizeof(SelectItem) * MAX_COLUMNS);
  do {
    if (st->nitems == MAX_COLUMNS)
      fail(p, "too many result columns");
    SelectItem *it = &st->items[st->nitems++];
    if (accept(p, "*")) {
      it->expr = new_expr(p, EX_STAR);
      continue;
    }
    it->expr = expr(p);
    if (accept(p, "AS"))
      it->alias = ident(p);
  } while (accept(p, ","));

  if (accept(p, "FROM"))
    st->table = ident(p);
  if (accept(p, "WHERE"))
    st->where = expr(p);
  if (accept(p, "GROUP")) {
    expect(p, "BY");
    st->group_by = expr_list(p, &st->ngroup);
  }
  if (accept(p, "ORDER")) {
    expect(p, "BY");
    st->order_by = arena_alloc(p->arena, sizeof(OrderItem) * MAX_COLUMNS);
    do {
      if (st->norder == MAX_COLUMNS)
        fail(p, "too many ORDER BY terms");
      OrderItem *o = &st->order_by[st->norder++];
      o->expr = expr(p);
      if (accept(p, "DESC"))
        o->desc = true;
      else
        accept(p, "ASC");
    } while (accept(p, ","));
  }
  if (accept(p, "LIMIT")) {
    if (p->tok.kind != T_NUM)
      fail(p, "LIMIT expects a number");
    st->limit = p->tok.num;
    next(p);
    if (accept(p, "OFFSET")) {
      if (p->tok.kind != T_NUM)
        fail(p, "OFFSET expects a number");
      st->offset = p->tok.num;
      next(p);
    }
  }
}

static void parse_update(Parser *p, Stmt *st) {
  st->kind = ST_UPDATE;
  st->table = ident(p);
  expect(p, "SET");
  do {
    if (st->nset == MAX_COLUMNS)
      fail(p, "too many assignments");
    st->set_cols[st->nset] = ident(p);
    expect(p, "=");
    st->set_vals[st->nset++] = expr(p);
  } while (accept(p, ","));
  if (accept(p, "WHERE"))
    st->where = expr(p);
}

static Stmt *statement(Parser *p) {
  Stmt *st = arena_alloc(p->arena, sizeof(Stmt));
  const char *start = p->tok.start;
  if (accept(p, "EXPLAIN")) {
    st->kind = ST_EXPLAIN;
    st->inner = statement(p);
  } else if (accept(p, "CREATE")) {
    parse_create(p, st, start);
  } else if (accept(p, "DROP")) {
    st->kind = ST_DROP;
    expect(p, "TABLE");
    if (accept(p, "IF")) {
      expect(p, "EXISTS");
      st->if_not_exists = true; // reused as "IF EXISTS"
    }
    st->table = ident(p);
  } else if (accept(p, "INSERT")) {
    parse_insert(p, st);
  } else if (accept(p, "SELECT")) {
    parse_select(p, st);
  } else if (accept(p, "UPDATE")) {
    parse_update(p, st);
  } else if (accept(p, "DELETE")) {
    st->kind = ST_DELETE;
    expect(p, "FROM");
    st->table = ident(p);
    if (accept(p, "WHERE"))
      st->where = expr(p);
  } else if (accept(p, "BEGIN")) {
    st->kind = ST_BEGIN;
    accept(p, "TRANSACTION");
  } else if (accept(p, "COMMIT") || accept(p, "END")) {
    st->kind = ST_COMMIT;
    accept(p, "TRANSACTION");
  } else if (accept(p, "ROLLBACK")) {
    st->kind = ST_ROLLBACK;
    accept(p, "TRANSACTION");
  } else {
    fail(p, "syntax error");
  }
  return st;
}

Stmt *parse_statement(Arena *a, const char **sql, char **err) {
  Parser p = {.arena = a, .src = *sql, .pos = *sql};
  *err = NULL;
  if (setjmp(p.on_error)) {
    *err = p.err;
    // Skip to the next ';' so the following statement can still run.
    const char *s = p.pos;
    while (*s && *s != ';')
      s++;
    *sql = *s ? s + 1 : s;
    return NULL;
  }
  next(&p);
  while (accept(&p, ";"))
    ;
  if (p.tok.kind == T_EOF) {
    *sql = p.pos;
    return NULL;
  }
  Stmt *st = statement(&p);
  if (!accept(&p, ";") && p.tok.kind != T_EOF)
    fail(&p, "syntax error");
  *sql = p.tok.start; // the first token after this statement
  return st;
}

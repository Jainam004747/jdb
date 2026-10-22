// The jdb shell.
//
//   jdb shop.db                   interactive prompt (tables are drawn as boxes)
//   jdb shop.db < script.sql      run a script (output in SQLite's "list" format: a|b|c)
//   jdb shop.db -c "SELECT 1"     run one command
//
// Dot-commands: .tables .schema .mode .headers .stats .check .help .quit

#include "jdb.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static OutputOptions opt = {OUT_LIST, false, NULL};
static bool interactive;
static int errors;

static void run_sql(Database *db, const char *sql) {
  const char *p = sql;
  for (;;) {
    Arena a = {0};
    char *err = NULL;
    Stmt *st = parse_statement(&a, &p, &err);
    if (!st && !err) {
      arena_free(&a);
      return;
    }
    if (st && execute(db, &a, st, &opt, &err) == 0) {
      arena_free(&a);
      continue;
    }
    fprintf(stderr, "Error: %s\n", err);
    fflush(stderr);
    errors++;
    arena_free(&a);
  }
}

static void print_help(void) {
  printf(".tables            list tables\n"
         ".schema [table]    show CREATE TABLE statements\n"
         ".mode list|table   output format\n"
         ".headers on|off    show column names in list mode\n"
         ".stats             buffer pool and I/O statistics\n"
         ".check             verify every B+tree's structure\n"
         ".quit              exit\n"
         "SQL: CREATE TABLE, DROP TABLE, INSERT, SELECT (WHERE, GROUP BY, ORDER BY, LIMIT),\n"
         "     UPDATE, DELETE, BEGIN/COMMIT/ROLLBACK, EXPLAIN SELECT ...\n");
}

// Returns false to exit.
static bool dot_command(Database *db, char *line) {
  char *cmd = strtok(line, " \t\n");
  char *arg = strtok(NULL, " \t\n");
  if (!strcmp(cmd, ".quit") || !strcmp(cmd, ".exit"))
    return false;
  if (!strcmp(cmd, ".help")) {
    print_help();
  } else if (!strcmp(cmd, ".tables")) {
    for (int i = 0; i < db->ntables; i++)
      printf("%s%s", i ? "  " : "", db->tables[i]->name);
    if (db->ntables)
      printf("\n");
  } else if (!strcmp(cmd, ".schema")) {
    for (int i = 0; i < db->ntables; i++)
      if (!arg || !strcasecmp(arg, db->tables[i]->name))
        printf("%s;\n", db->tables[i]->sql);
  } else if (!strcmp(cmd, ".mode") && arg) {
    opt.mode = !strcmp(arg, "table") ? OUT_TABLE : OUT_LIST;
  } else if (!strcmp(cmd, ".headers") && arg) {
    opt.headers = !strcmp(arg, "on");
  } else if (!strcmp(cmd, ".stats")) {
    Pager *p = db->pager;
    double hit = p->hits + p->misses ? 100.0 * (double)p->hits / (double)(p->hits + p->misses) : 0;
    printf("pages in file:      %u (%u KB)\n", p->page_count, p->page_count * PAGE_SIZE / 1024);
    printf("buffer pool:        %d frames cached (capacity %d)\n", p->nframes, p->capacity);
    printf("page requests:      %llu hits, %llu misses (%.1f%% hit rate)\n", (unsigned long long)p->hits,
           (unsigned long long)p->misses, hit);
    printf("disk:               %llu page reads, %llu page writes, %llu evictions\n",
           (unsigned long long)p->reads, (unsigned long long)p->writes, (unsigned long long)p->evictions);
    printf("transactions:       %llu committed, %llu WAL frames written\n", (unsigned long long)p->commits,
           (unsigned long long)p->wal_frames);
    for (int i = 0; i < db->ntables; i++) {
      BTree t = {p, db->tables[i]->root};
      printf("table %-12s B+tree depth %d, root page %u\n", db->tables[i]->name, btree_depth(&t),
             db->tables[i]->root);
    }
  } else if (!strcmp(cmd, ".check")) {
    bool ok = true;
    for (int i = 0; i < db->ntables; i++) {
      BTree t = {db->pager, db->tables[i]->root};
      long count;
      char err[256] = "";
      bool good = btree_check(&t, &count, err, sizeof err);
      printf("%s: %s (%ld rows)%s%s\n", db->tables[i]->name, good ? "ok" : "CORRUPT", count, good ? "" : " ", err);
      ok &= good;
    }
    if (!ok)
      errors++;
  } else {
    fprintf(stderr, "Error: unknown command %s (try .help)\n", cmd);
    errors++;
  }
  return true;
}

// True when `buf` ends with a ';' that is not inside a quoted string.
static bool statement_complete(const char *buf) {
  char quote = 0;
  bool complete = false;
  for (const char *s = buf; *s; s++) {
    if (quote) {
      if (*s == quote)
        quote = 0;
      continue;
    }
    if (*s == '\'' || *s == '"')
      quote = *s;
    else if (*s == ';')
      complete = true;
    else if (*s == '-' && s[1] == '-')
      while (*s && s[1] != '\n')
        s++;
    else if (!strchr(" \t\r\n", *s))
      complete = false;
  }
  return complete && !quote;
}

static void repl(Database *db) {
  char *buf = NULL;
  size_t buflen = 0;
  char line[4096];
  if (interactive)
    printf("jdb: a database built from scratch. Type .help for help, .quit to exit.\n");
  for (;;) {
    if (interactive) {
      printf(buflen ? "...> " : "jdb> ");
      fflush(stdout);
    }
    if (!fgets(line, sizeof line, stdin))
      break;
    if (buflen == 0 && line[0] == '.') {
      if (!dot_command(db, line))
        break;
      continue;
    }
    size_t n = strlen(line);
    buf = realloc(buf, buflen + n + 1);
    memcpy(buf + buflen, line, n + 1);
    buflen += n;
    if (statement_complete(buf)) {
      run_sql(db, buf);
      buflen = 0;
      buf[0] = '\0';
    }
  }
  if (buflen > 0)
    run_sql(db, buf); // a final statement without ';'
  free(buf);
}

int main(int argc, char **argv) {
  const char *path = NULL;
  const char *command = NULL;
  int cache_pages = 256;
  opt.out = stdout;
  interactive = isatty(0);
  if (interactive)
    opt.mode = OUT_TABLE;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-c") && i + 1 < argc)
      command = argv[++i];
    else if (!strcmp(argv[i], "-list"))
      opt.mode = OUT_LIST;
    else if (!strcmp(argv[i], "-table"))
      opt.mode = OUT_TABLE;
    else if (!strcmp(argv[i], "-header"))
      opt.headers = true;
    else if (!strcmp(argv[i], "--cache") && i + 1 < argc)
      cache_pages = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      printf("usage: jdb [-c SQL] [-list|-table] [-header] [--cache PAGES] [FILE]\n");
      return 0;
    } else
      path = argv[i];
  }

  char tmp_path[] = "/tmp/jdb-XXXXXX";
  if (!path) { // no file given: use a throwaway database, like SQLite's in-memory mode
    int fd = mkstemp(tmp_path);
    close(fd);
    unlink(tmp_path);
    path = tmp_path;
    if (interactive)
      printf("(using a temporary database; pass a file name to keep your data)\n");
  }

  Database *db = db_open(path, cache_pages);
  if (command)
    run_sql(db, command);
  else
    repl(db);
  db_close(db);
  if (path == tmp_path)
    unlink(tmp_path);
  return errors ? 1 : 0;
}

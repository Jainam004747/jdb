// The pager: turns one file into an array of 4 KB pages, caches pages in a buffer pool, and
// makes multi-page changes atomic with a write-ahead log.
//
// FILE LAYOUT
//   page 0       header: magic "JDB1", version, page count, then the catalog (table list)
//   page 1..N    B+tree nodes
//
// BUFFER POOL
//   Reading a page from disk is ~1000x slower than reading memory, so recently used pages are
//   kept in "frames". When the pool is full, the least recently used clean, unpinned page is
//   evicted. A page is "pinned" while code holds a pointer to it, so it cannot vanish.
//
// TRANSACTIONS: NO-STEAL / REDO-ONLY WAL
//   Changed ("dirty") pages are never written to the database file before COMMIT ("no-steal").
//   So ROLLBACK is trivial: throw the dirty frames away.
//   COMMIT is:
//     1. append every dirty page, then a COMMIT record, to the WAL file      (sequential I/O)
//     2. fsync the WAL                    <- the commit point: from here the change is durable
//     3. write the pages into the database file, fsync it
//     4. empty the WAL
//   If the process crashes before step 2, the transaction never happened. If it crashes after
//   step 2, pager_recover() finds the complete transaction in the WAL on the next start and
//   copies its pages into the database ("redo"). Each WAL frame has a CRC32, so a frame that
//   was only half written by a crash is detected and ignored.

#include "jdb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define DB_MAGIC "JDB1"
#define DB_VERSION 1
#define WAL_MAGIC 0x4A57414Cu // "JWAL"
#define WAL_PAGE 1
#define WAL_COMMIT 2

typedef struct {
  uint32_t magic;
  uint32_t type;
  uint64_t txn;
  uint32_t pgno;   // PAGE: which page; COMMIT: the page count after the transaction
  uint32_t crc;    // CRC32 of the fields above plus, for PAGE frames, the page data
} WalFrameHeader;

static uint32_t frame_crc(WalFrameHeader *h, const uint8_t *page) {
  uint32_t c = crc32(0, h, offsetof(WalFrameHeader, crc));
  if (page)
    c = crc32(c, page, PAGE_SIZE);
  return c;
}

static void write_all(int fd, const void *buf, size_t n, off_t off) {
  const uint8_t *p = buf;
  while (n > 0) {
    ssize_t w = off >= 0 ? pwrite(fd, p, n, off) : write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      die("write failed: %s", strerror(errno));
    }
    p += w;
    n -= (size_t)w;
    if (off >= 0)
      off += w;
  }
}

static void read_page_from_disk(Pager *p, Pgno pgno, uint8_t *buf) {
  ssize_t n = pread(p->fd, buf, PAGE_SIZE, (off_t)pgno * PAGE_SIZE);
  if (n < 0)
    die("read failed: %s", strerror(errno));
  if (n < PAGE_SIZE)
    memset(buf + n, 0, PAGE_SIZE - (size_t)n); // past the end of the file: zeros
  p->reads++;
}

static uint32_t get_u32(const uint8_t *b) {
  uint32_t v;
  memcpy(&v, b, 4);
  return v;
}

static void put_u32(uint8_t *b, uint32_t v) {
  memcpy(b, &v, 4);
}

// Crash injection for tests: JDB_CRASH=mid-wal or after-wal makes commit stop at that point,
// as if the power had failed.
static void maybe_crash(const char *point) {
  const char *c = getenv("JDB_CRASH");
  if (c && strcmp(c, point) == 0) {
    fprintf(stderr, "jdb: simulated crash at %s\n", point);
    _exit(3);
  }
}

// ---- buffer pool -----------------------------------------------------------------

static Frame *lookup(Pager *p, Pgno pgno) {
  for (Frame *f = p->hash[pgno % 1024]; f; f = f->hash_next)
    if (f->pgno == pgno)
      return f;
  return NULL;
}

static void hash_remove(Pager *p, Frame *f) {
  Frame **pp = &p->hash[f->pgno % 1024];
  while (*pp && *pp != f)
    pp = &(*pp)->hash_next;
  if (*pp)
    *pp = f->hash_next;
}

static void hash_insert(Pager *p, Frame *f) {
  f->hash_next = p->hash[f->pgno % 1024];
  p->hash[f->pgno % 1024] = f;
}

// Finds a frame to hold a new page: a free slot, or the least recently used page that is
// clean and unpinned. Dirty pages cannot be evicted (no-steal), so if every page is dirty the
// pool grows past its soft capacity until the transaction commits.
static Frame *get_free_frame(Pager *p) {
  if (p->nframes < p->capacity) {
    Frame *f = calloc(1, sizeof(Frame));
    if (!f)
      die("out of memory");
    p->frames = realloc(p->frames, sizeof(Frame *) * (size_t)(p->nframes + 1));
    p->frames[p->nframes++] = f;
    return f;
  }
  Frame *victim = NULL;
  for (int i = 0; i < p->nframes; i++) {
    Frame *f = p->frames[i];
    if (f->pins == 0 && !f->dirty && (!victim || f->last_used < victim->last_used))
      victim = f;
  }
  if (victim) {
    hash_remove(p, victim);
    p->evictions++;
    return victim;
  }
  p->capacity++; // every page is in use or dirty: grow temporarily
  return get_free_frame(p);
}

uint8_t *pager_get(Pager *p, Pgno pgno) {
  if (pgno >= p->page_count)
    die("page %u out of range (page count %u): database is corrupt", pgno, p->page_count);
  Frame *f = lookup(p, pgno);
  if (f) {
    p->hits++;
  } else {
    p->misses++;
    f = get_free_frame(p);
    f->pgno = pgno;
    f->dirty = false;
    f->pins = 0;
    read_page_from_disk(p, pgno, f->data);
    hash_insert(p, f);
  }
  f->pins++;
  f->last_used = ++p->clock;
  return f->data;
}

void pager_unpin(Pager *p, Pgno pgno) {
  Frame *f = lookup(p, pgno);
  if (!f || f->pins <= 0)
    die("internal error: unpin of page %u that is not pinned", pgno);
  f->pins--;
}

void pager_mark_dirty(Pager *p, Pgno pgno) {
  Frame *f = lookup(p, pgno);
  if (!f)
    die("internal error: mark_dirty of page %u that is not cached", pgno);
  f->dirty = true;
}

// New pages go at the end of the file. (A production database would also reuse freed pages
// from a free list; jdb leaves dropped pages zeroed instead. See docs/DECISIONS.md.)
Pgno pager_allocate(Pager *p) {
  Pgno pgno = p->page_count++;
  uint8_t *header = pager_get(p, 0);
  put_u32(header + 8, p->page_count);
  pager_mark_dirty(p, 0);
  pager_unpin(p, 0);

  Frame *f = get_free_frame(p);
  f->pgno = pgno;
  f->pins = 1;
  f->dirty = true;
  f->last_used = ++p->clock;
  memset(f->data, 0, PAGE_SIZE);
  hash_insert(p, f);
  return pgno;
}

// ---- transactions ----------------------------------------------------------------

void pager_begin(Pager *p) {
  p->in_txn = true;
}

void pager_commit(Pager *p) {
  p->in_txn = false;
  int ndirty = 0;
  for (int i = 0; i < p->nframes; i++)
    if (p->frames[i]->dirty)
      ndirty++;
  if (ndirty == 0)
    return;

  uint64_t txn = ++p->txn_id;
  // 1. Append every dirty page to the WAL.
  if (lseek(p->wal_fd, 0, SEEK_END) < 0)
    die("lseek failed: %s", strerror(errno));
  int written = 0;
  for (int i = 0; i < p->nframes; i++) {
    Frame *f = p->frames[i];
    if (!f->dirty)
      continue;
    WalFrameHeader h = {WAL_MAGIC, WAL_PAGE, txn, f->pgno, 0};
    h.crc = frame_crc(&h, f->data);
    write_all(p->wal_fd, &h, sizeof h, -1);
    write_all(p->wal_fd, f->data, PAGE_SIZE, -1);
    p->wal_frames++;
    if (++written == ndirty / 2 + 1)
      maybe_crash("mid-wal");
  }
  // 2. The commit record, then fsync: the moment the transaction becomes durable.
  WalFrameHeader commit = {WAL_MAGIC, WAL_COMMIT, txn, p->page_count, 0};
  commit.crc = frame_crc(&commit, NULL);
  write_all(p->wal_fd, &commit, sizeof commit, -1);
  if (fsync(p->wal_fd) != 0)
    die("fsync of WAL failed: %s", strerror(errno));
  maybe_crash("after-wal");

  // 3. Checkpoint: copy the pages into the database file.
  for (int i = 0; i < p->nframes; i++) {
    Frame *f = p->frames[i];
    if (!f->dirty)
      continue;
    write_all(p->fd, f->data, PAGE_SIZE, (off_t)f->pgno * PAGE_SIZE);
    p->writes++;
    f->dirty = false;
  }
  if (fsync(p->fd) != 0)
    die("fsync of database failed: %s", strerror(errno));

  // 4. The WAL is no longer needed.
  if (ftruncate(p->wal_fd, 0) != 0)
    die("truncating the WAL failed: %s", strerror(errno));
  p->commits++;
}

void pager_rollback(Pager *p) {
  p->in_txn = false;
  // Nothing dirty ever reached the file, so dropping the dirty frames restores the old state.
  int kept = 0;
  for (int i = 0; i < p->nframes; i++) {
    Frame *f = p->frames[i];
    if (f->dirty) {
      if (f->pins)
        die("internal error: rollback while page %u is pinned", f->pgno);
      hash_remove(p, f);
      free(f);
    } else {
      p->frames[kept++] = f;
    }
  }
  p->nframes = kept;
  uint8_t header[PAGE_SIZE];
  read_page_from_disk(p, 0, header);
  p->page_count = get_u32(header + 8);
}

// ---- recovery ----------------------------------------------------------------------

typedef struct {
  Pgno pgno;
  uint8_t data[PAGE_SIZE];
} PendingPage;

int pager_recover(Pager *p) {
  off_t size = lseek(p->wal_fd, 0, SEEK_END);
  if (size <= 0)
    return 0;
  lseek(p->wal_fd, 0, SEEK_SET);

  PendingPage *pending = NULL;
  int npending = 0;
  int recovered = 0;
  uint64_t current_txn = 0;
  for (;;) {
    WalFrameHeader h;
    if (read(p->wal_fd, &h, sizeof h) != (ssize_t)sizeof h || h.magic != WAL_MAGIC)
      break;
    if (h.type == WAL_PAGE) {
      PendingPage pg;
      if (read(p->wal_fd, pg.data, PAGE_SIZE) != PAGE_SIZE || frame_crc(&h, pg.data) != h.crc)
        break; // torn write: the crash happened while this frame was being written
      if (h.txn != current_txn) {
        npending = 0; // a new transaction starts; an earlier one without COMMIT is discarded
        current_txn = h.txn;
      }
      pg.pgno = h.pgno;
      pending = realloc(pending, sizeof(PendingPage) * (size_t)(npending + 1));
      pending[npending++] = pg;
    } else if (h.type == WAL_COMMIT && frame_crc(&h, NULL) == h.crc && h.txn == current_txn) {
      for (int i = 0; i < npending; i++)
        write_all(p->fd, pending[i].data, PAGE_SIZE, (off_t)pending[i].pgno * PAGE_SIZE);
      npending = 0;
      recovered++;
      if (h.txn > p->txn_id)
        p->txn_id = h.txn;
    } else {
      break;
    }
  }
  free(pending);
  if (fsync(p->fd) != 0)
    die("fsync failed during recovery: %s", strerror(errno));
  if (ftruncate(p->wal_fd, 0) != 0)
    die("truncating the WAL failed: %s", strerror(errno));
  return recovered;
}

// ---- open / close -----------------------------------------------------------------

Pager *pager_open(const char *path, int cache_pages) {
  Pager *p = calloc(1, sizeof(Pager));
  p->path = strdup(path);
  p->capacity = cache_pages < 8 ? 8 : cache_pages;
  p->fd = open(path, O_RDWR | O_CREAT, 0644);
  if (p->fd < 0)
    die("cannot open %s: %s", path, strerror(errno));
  // One process at a time: a second jdb on the same file gets a clear error, not corruption.
  if (flock(p->fd, LOCK_EX | LOCK_NB) != 0)
    die("%s is locked by another jdb process", path);

  size_t wlen = strlen(path) + 5;
  p->wal_path = malloc(wlen);
  snprintf(p->wal_path, wlen, "%s-wal", path);
  p->wal_fd = open(p->wal_path, O_RDWR | O_CREAT, 0644);
  if (p->wal_fd < 0)
    die("cannot open %s: %s", p->wal_path, strerror(errno));

  struct stat st;
  fstat(p->fd, &st);
  if (st.st_size == 0) {
    // A brand-new database: write the header page directly.
    uint8_t header[PAGE_SIZE] = {0};
    memcpy(header, DB_MAGIC, 4);
    put_u32(header + 4, DB_VERSION);
    put_u32(header + 8, 1);
    write_all(p->fd, header, PAGE_SIZE, 0);
    fsync(p->fd);
  }

  int recovered = pager_recover(p);
  if (recovered > 0)
    fprintf(stderr, "jdb: recovered %d committed transaction%s from the write-ahead log\n",
            recovered, recovered == 1 ? "" : "s");

  uint8_t header[PAGE_SIZE];
  read_page_from_disk(p, 0, header);
  if (memcmp(header, DB_MAGIC, 4) != 0)
    die("%s is not a jdb database", path);
  if (get_u32(header + 4) != DB_VERSION)
    die("%s uses unsupported format version %u", path, get_u32(header + 4));
  p->page_count = get_u32(header + 8);
  p->reads = 0;
  return p;
}

void pager_close(Pager *p) {
  if (p->in_txn) {
    fprintf(stderr, "jdb: uncommitted transaction rolled back\n");
    pager_rollback(p);
  }
  pager_commit(p); // autocommit leftovers (normally nothing is dirty here)
  for (int i = 0; i < p->nframes; i++)
    free(p->frames[i]);
  free(p->frames);
  close(p->wal_fd);
  unlink(p->wal_path); // an empty WAL is not needed between sessions
  close(p->fd);
  free(p->path);
  free(p->wal_path);
  free(p);
}

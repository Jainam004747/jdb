// A B+tree stored in pages. Every table is one B+tree mapping rowid -> encoded row.
//
//                        [ internal: 40 | 80 ]              (keys + child page numbers)
//                       /         |          \  (children)
//          [leaf 1..39] -> [leaf 40..79] -> [leaf 80..]      (rows, linked left to right)
//
// * Leaves hold the data; internal nodes only route the search. With 4 KB pages an internal
//   node has up to 340 children, so a million rows is only about 3 levels deep: any row is
//   found in about 3 page reads.
// * Leaves are linked, so a range scan (WHERE id BETWEEN ...) walks sideways without going
//   back up the tree.
// * When a node is full it SPLITS in two, and the parent gains a key. When the root splits the
//   tree grows one level taller. This keeps the tree balanced: every leaf is at the same depth.
//
// PAGE LAYOUT (both node types)
//   0   u8   type: 1 = leaf, 2 = internal
//   2   u16  number of cells
//   4   u16  leaf: start of the cell-content area (it grows down from the end of the page)
//   8   u32  leaf: next leaf (0 = none)   internal: rightmost child
//   12  ...  leaf: u16 slot array (offsets of cells, sorted by key)
//            internal: cells of (u32 child, i64 key): child holds keys < key
//
// Leaf cell: i64 key, u16 length, payload bytes. This "slotted page" design lets rows of
// different sizes share a page, and keeps the slot array sorted without moving the rows.

#include "jdb.h"

#include <string.h>

enum { LEAF = 1, INTERNAL = 2 };
#define HEADER 12
#define INTERNAL_CELL 12
#define MAX_INTERNAL_KEYS ((PAGE_SIZE - HEADER) / INTERNAL_CELL)

// ---- little helpers to read/write fields (memcpy avoids unaligned access) ----

static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int64_t i64(const uint8_t *p) { int64_t v; memcpy(&v, p, 8); return v; }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, int64_t v) { memcpy(p, &v, 8); }

static int node_type(const uint8_t *pg) { return pg[0]; }
static int ncells(const uint8_t *pg) { return u16(pg + 2); }
static void set_ncells(uint8_t *pg, int n) { put16(pg + 2, (uint16_t)n); }
static int content_start(const uint8_t *pg) { int v = u16(pg + 4); return v ? v : PAGE_SIZE; }
static void set_content_start(uint8_t *pg, int v) { put16(pg + 4, (uint16_t)(v == PAGE_SIZE ? 0 : v)); }
static Pgno right_ptr(const uint8_t *pg) { return u32(pg + 8); }
static void set_right_ptr(uint8_t *pg, Pgno v) { put32(pg + 8, v); }

// Leaf accessors
static int slot_off(const uint8_t *pg, int i) { return u16(pg + HEADER + 2 * i); }
static int64_t leaf_key(const uint8_t *pg, int i) { return i64(pg + slot_off(pg, i)); }
static int leaf_len(const uint8_t *pg, int i) { return u16(pg + slot_off(pg, i) + 8); }
static const uint8_t *leaf_val(const uint8_t *pg, int i) { return pg + slot_off(pg, i) + 10; }
static int leaf_free(const uint8_t *pg) { return content_start(pg) - (HEADER + 2 * ncells(pg)); }

// Internal accessors
static Pgno int_child(const uint8_t *pg, int i) {
  return i == ncells(pg) ? right_ptr(pg) : u32(pg + HEADER + INTERNAL_CELL * i);
}
static int64_t int_key(const uint8_t *pg, int i) { return i64(pg + HEADER + INTERNAL_CELL * i + 4); }

static void init_leaf(uint8_t *pg) {
  memset(pg, 0, PAGE_SIZE);
  pg[0] = LEAF;
  set_content_start(pg, PAGE_SIZE);
}

Pgno btree_create(Pager *p) {
  Pgno root = pager_allocate(p);
  init_leaf(pager_get(p, root));
  pager_unpin(p, root);
  pager_unpin(p, root); // allocate() also pinned it
  return root;
}

// Binary search in a leaf: index of the first key >= key.
static int leaf_search(const uint8_t *pg, int64_t key) {
  int lo = 0, hi = ncells(pg);
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (leaf_key(pg, mid) < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

// Binary search in an internal node: which child may contain `key`.
static int internal_search(const uint8_t *pg, int64_t key) {
  int lo = 0, hi = ncells(pg);
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (key < int_key(pg, mid))
      hi = mid;
    else
      lo = mid + 1;
  }
  return lo; // child index; == ncells means the rightmost child
}

// Rewrites a leaf so all free space is in one block (deleted cells leave holes).
static void leaf_defragment(uint8_t *pg) {
  uint8_t copy[PAGE_SIZE];
  memcpy(copy, pg, PAGE_SIZE);
  int n = ncells(copy);
  int pos = PAGE_SIZE;
  for (int i = 0; i < n; i++) {
    int size = 10 + leaf_len(copy, i);
    pos -= size;
    memcpy(pg + pos, copy + slot_off(copy, i), (size_t)size);
    put16(pg + HEADER + 2 * i, (uint16_t)pos);
  }
  set_content_start(pg, pos);
}

static void leaf_insert_at(uint8_t *pg, int idx, int64_t key, const uint8_t *val, int len) {
  int n = ncells(pg);
  int pos = content_start(pg) - (10 + len);
  put64(pg + pos, key);
  put16(pg + pos + 8, (uint16_t)len);
  memcpy(pg + pos + 10, val, (size_t)len);
  memmove(pg + HEADER + 2 * (idx + 1), pg + HEADER + 2 * idx, (size_t)(2 * (n - idx)));
  put16(pg + HEADER + 2 * idx, (uint16_t)pos);
  set_content_start(pg, pos);
  set_ncells(pg, n + 1);
}

typedef struct {
  int64_t key;
  int len;
  const uint8_t *val;
} CellRef;

// Splits a full leaf. All cells plus the new one are divided by size into the old page (left)
// and a new page (right). The first key of the right page is passed up to the parent.
static void leaf_split(BTree *t, Pgno left, uint8_t *pg, int idx, int64_t key, const uint8_t *val,
                       int len, int64_t *sep, Pgno *new_right) {
  uint8_t copy[PAGE_SIZE];
  memcpy(copy, pg, PAGE_SIZE);
  int n = ncells(copy);
  CellRef cells[PAGE_SIZE / 10 + 1];
  int total = 0;
  for (int i = 0, j = 0; i <= n; i++) {
    if (i == idx)
      cells[i] = (CellRef){key, len, val};
    else {
      cells[i] = (CellRef){leaf_key(copy, j), leaf_len(copy, j), leaf_val(copy, j)};
      j++;
    }
    total += 12 + cells[i].len;
  }
  int count = n + 1;
  int split = 0, acc = 0;
  if (idx == n && right_ptr(copy) == 0) {
    // Appending past the last key of the rightmost leaf (ids 1, 2, 3, ...): keep the old page
    // full and start the new page with just the new row. A 50/50 split here would leave every
    // page half empty forever, because no key will ever be inserted into the left half again.
    split = n;
  } else {
    while (split < count - 1 && acc + 12 + cells[split].len <= total / 2) {
      acc += 12 + cells[split].len;
      split++;
    }
    if (split == 0)
      split = 1;
  }

  Pgno right = pager_allocate(t->pager); // pinned
  uint8_t *rpg = pager_get(t->pager, right);
  init_leaf(rpg);
  for (int i = split; i < count; i++)
    leaf_insert_at(rpg, i - split, cells[i].key, cells[i].val, cells[i].len);
  set_right_ptr(rpg, right_ptr(copy)); // right takes over the old "next" link

  init_leaf(pg);
  for (int i = 0; i < split; i++)
    leaf_insert_at(pg, i, cells[i].key, cells[i].val, cells[i].len);
  set_right_ptr(pg, right);

  pager_mark_dirty(t->pager, left);
  pager_mark_dirty(t->pager, right);
  *sep = cells[split].key;
  *new_right = right;
  pager_unpin(t->pager, right);
  pager_unpin(t->pager, right);
}

// Loads an internal node into arrays: keys[0..n-1], children[0..n].
static int load_internal(const uint8_t *pg, int64_t *keys, Pgno *children) {
  int n = ncells(pg);
  for (int i = 0; i < n; i++) {
    keys[i] = int_key(pg, i);
    children[i] = int_child(pg, i);
  }
  children[n] = right_ptr(pg);
  return n;
}

static void store_internal(uint8_t *pg, const int64_t *keys, const Pgno *children, int n) {
  memset(pg, 0, PAGE_SIZE);
  pg[0] = INTERNAL;
  set_ncells(pg, n);
  for (int i = 0; i < n; i++) {
    put32(pg + HEADER + INTERNAL_CELL * i, children[i]);
    put64(pg + HEADER + INTERNAL_CELL * i + 4, keys[i]);
  }
  set_right_ptr(pg, children[n]);
}

// Returns -1 if the key already exists, 0 if inserted, 1 if the node split
// (then *sep and *new_right describe the new right sibling for the parent).
static int insert_rec(BTree *t, Pgno pgno, int64_t key, const uint8_t *val, int len, int64_t *sep,
                      Pgno *new_right) {
  uint8_t *pg = pager_get(t->pager, pgno);
  int result = 0;

  if (node_type(pg) == LEAF) {
    int idx = leaf_search(pg, key);
    if (idx < ncells(pg) && leaf_key(pg, idx) == key) {
      result = -1;
    } else {
      int need = 10 + len + 2;
      if (leaf_free(pg) < need)
        leaf_defragment(pg);
      if (leaf_free(pg) >= need) {
        leaf_insert_at(pg, idx, key, val, len);
        pager_mark_dirty(t->pager, pgno);
      } else {
        leaf_split(t, pgno, pg, idx, key, val, len, sep, new_right);
        result = 1;
      }
    }
    pager_unpin(t->pager, pgno);
    return result;
  }

  int idx = internal_search(pg, key);
  Pgno child = int_child(pg, idx);
  int64_t child_sep;
  Pgno child_right;
  int r = insert_rec(t, child, key, val, len, &child_sep, &child_right);
  if (r != 1) {
    pager_unpin(t->pager, pgno);
    return r;
  }

  // The child split: add (child_sep, child_right) to this node, right after `child`.
  int64_t keys[MAX_INTERNAL_KEYS + 1];
  Pgno children[MAX_INTERNAL_KEYS + 2];
  int n = load_internal(pg, keys, children);
  memmove(keys + idx + 1, keys + idx, sizeof(int64_t) * (size_t)(n - idx));
  memmove(children + idx + 2, children + idx + 1, sizeof(Pgno) * (size_t)(n - idx));
  keys[idx] = child_sep;
  children[idx + 1] = child_right;
  n++;

  if (n <= MAX_INTERNAL_KEYS) {
    store_internal(pg, keys, children, n);
    pager_mark_dirty(t->pager, pgno);
    pager_unpin(t->pager, pgno);
    return 0;
  }

  // This node is full too: split it. The middle key moves UP (it is not kept in either half).
  int mid = n / 2;
  Pgno right = pager_allocate(t->pager);
  uint8_t *rpg = pager_get(t->pager, right);
  store_internal(rpg, keys + mid + 1, children + mid + 1, n - mid - 1);
  store_internal(pg, keys, children, mid);
  pager_mark_dirty(t->pager, pgno);
  pager_mark_dirty(t->pager, right);
  pager_unpin(t->pager, right);
  pager_unpin(t->pager, right);
  pager_unpin(t->pager, pgno);
  *sep = keys[mid];
  *new_right = right;
  return 1;
}

bool btree_insert(BTree *t, int64_t key, const uint8_t *val, int len) {
  if (len > MAX_ROW_SIZE)
    die("internal error: row of %d bytes exceeds the maximum", len);
  int64_t sep;
  Pgno right;
  int r = insert_rec(t, t->root, key, val, len, &sep, &right);
  if (r == -1)
    return false;
  if (r == 1) {
    // The root split. The root's page number must not change (the catalog points to it), so
    // its contents move to a new page and the root becomes an internal node above both halves.
    Pgno left = pager_allocate(t->pager);
    uint8_t *lpg = pager_get(t->pager, left);
    uint8_t *rootpg = pager_get(t->pager, t->root);
    memcpy(lpg, rootpg, PAGE_SIZE);
    int64_t keys[1] = {sep};
    Pgno children[2] = {left, right};
    store_internal(rootpg, keys, children, 1);
    pager_mark_dirty(t->pager, left);
    pager_mark_dirty(t->pager, t->root);
    pager_unpin(t->pager, t->root);
    pager_unpin(t->pager, left);
    pager_unpin(t->pager, left);
  }
  return true;
}

// Descends from the root to the leaf that would contain `key`.
static Pgno find_leaf(BTree *t, int64_t key) {
  Pgno pgno = t->root;
  for (;;) {
    uint8_t *pg = pager_get(t->pager, pgno);
    if (node_type(pg) == LEAF) {
      pager_unpin(t->pager, pgno);
      return pgno;
    }
    Pgno child = int_child(pg, internal_search(pg, key));
    pager_unpin(t->pager, pgno);
    pgno = child;
  }
}

bool btree_find(BTree *t, int64_t key, uint8_t *out, int *len) {
  Pgno leaf = find_leaf(t, key);
  uint8_t *pg = pager_get(t->pager, leaf);
  int idx = leaf_search(pg, key);
  bool found = idx < ncells(pg) && leaf_key(pg, idx) == key;
  if (found && out) {
    *len = leaf_len(pg, idx);
    memcpy(out, leaf_val(pg, idx), (size_t)*len);
  }
  pager_unpin(t->pager, leaf);
  return found;
}

// Deletes by removing the slot. The cell's bytes become a hole that defragmentation reclaims.
// Underfull leaves are not merged (a simplification; see docs/DECISIONS.md).
bool btree_delete(BTree *t, int64_t key) {
  Pgno leaf = find_leaf(t, key);
  uint8_t *pg = pager_get(t->pager, leaf);
  int idx = leaf_search(pg, key);
  bool found = idx < ncells(pg) && leaf_key(pg, idx) == key;
  if (found) {
    int n = ncells(pg);
    memmove(pg + HEADER + 2 * idx, pg + HEADER + 2 * (idx + 1), (size_t)(2 * (n - idx - 1)));
    set_ncells(pg, n - 1);
    pager_mark_dirty(t->pager, leaf);
  }
  pager_unpin(t->pager, leaf);
  return found;
}

// ---- cursors: iterate over entries in key order -----------------------------------------

// Moves forward past empty leaves (left behind by deletes) until a cell or the end.
static void cursor_settle(Cursor *c) {
  while (c->valid) {
    uint8_t *pg = pager_get(c->tree->pager, c->page);
    int n = ncells(pg);
    Pgno next = right_ptr(pg);
    pager_unpin(c->tree->pager, c->page);
    if (c->slot < n)
      return;
    if (next == 0) {
      c->valid = false;
      return;
    }
    c->page = next;
    c->slot = 0;
  }
}

void btree_seek(BTree *t, int64_t key, Cursor *c) {
  c->tree = t;
  c->page = find_leaf(t, key);
  uint8_t *pg = pager_get(t->pager, c->page);
  c->slot = leaf_search(pg, key);
  pager_unpin(t->pager, c->page);
  c->valid = true;
  cursor_settle(c);
}

void btree_first(BTree *t, Cursor *c) {
  btree_seek(t, INT64_MIN, c);
}

void btree_next(Cursor *c) {
  c->slot++;
  cursor_settle(c);
}

int64_t cursor_key(Cursor *c) {
  uint8_t *pg = pager_get(c->tree->pager, c->page);
  int64_t k = leaf_key(pg, c->slot);
  pager_unpin(c->tree->pager, c->page);
  return k;
}

// The returned pointer stays valid until the page is evicted; callers copy it out at once.
const uint8_t *cursor_value(Cursor *c, int *len) {
  uint8_t *pg = pager_get(c->tree->pager, c->page);
  *len = leaf_len(pg, c->slot);
  const uint8_t *v = leaf_val(pg, c->slot);
  pager_unpin(c->tree->pager, c->page);
  return v;
}

int64_t btree_max_key(BTree *t) {
  // Fast path: follow the rightmost children down to the last leaf.
  Pgno pgno = t->root;
  for (;;) {
    uint8_t *pg = pager_get(t->pager, pgno);
    if (node_type(pg) == LEAF) {
      int n = ncells(pg);
      int64_t k = n > 0 ? leaf_key(pg, n - 1) : INT64_MIN;
      pager_unpin(t->pager, pgno);
      if (k != INT64_MIN)
        return k;
      break;
    }
    Pgno next = right_ptr(pg);
    pager_unpin(t->pager, pgno);
    pgno = next;
  }
  // The last leaf is empty (rows were deleted): fall back to a full scan.
  int64_t max = 0;
  Cursor c;
  for (btree_first(t, &c); c.valid; btree_next(&c))
    max = cursor_key(&c);
  return max;
}

int btree_depth(BTree *t) {
  int depth = 1;
  Pgno pgno = t->root;
  for (;;) {
    uint8_t *pg = pager_get(t->pager, pgno);
    bool leaf = node_type(pg) == LEAF;
    Pgno child = leaf ? 0 : int_child(pg, 0);
    pager_unpin(t->pager, pgno);
    if (leaf)
      return depth;
    depth++;
    pgno = child;
  }
}

static void free_rec(Pager *p, Pgno pgno) {
  uint8_t *pg = pager_get(p, pgno);
  if (node_type(pg) == INTERNAL)
    for (int i = 0; i <= ncells(pg); i++)
      free_rec(p, int_child(pg, i));
  memset(pg, 0, PAGE_SIZE);
  pager_mark_dirty(p, pgno);
  pager_unpin(p, pgno);
}

void btree_free_all(BTree *t) {
  free_rec(t->pager, t->root);
}

// ---- self-check: proves the structure is a valid B+tree -----------------------------------

typedef struct {
  long count;
  int leaf_depth;
  Pgno prev_leaf;
  int64_t prev_key;
  bool have_prev;
  char *err;
  size_t errlen;
} CheckState;

static bool check_rec(BTree *t, Pgno pgno, int64_t lo, int64_t hi, bool has_lo, bool has_hi, int depth,
                      CheckState *s) {
  uint8_t pg[PAGE_SIZE];
  memcpy(pg, pager_get(t->pager, pgno), PAGE_SIZE);
  pager_unpin(t->pager, pgno);
  int n = ncells(pg);
  if (node_type(pg) == LEAF) {
    if (s->leaf_depth == 0)
      s->leaf_depth = depth;
    if (s->leaf_depth != depth) {
      snprintf(s->err, s->errlen, "leaf %u at depth %d, others at %d", pgno, depth, s->leaf_depth);
      return false;
    }
    if (s->prev_leaf) {
      uint8_t prev[PAGE_SIZE];
      memcpy(prev, pager_get(t->pager, s->prev_leaf), PAGE_SIZE);
      pager_unpin(t->pager, s->prev_leaf);
      if (right_ptr(prev) != pgno) {
        snprintf(s->err, s->errlen, "leaf %u does not link to next leaf %u", s->prev_leaf, pgno);
        return false;
      }
    }
    s->prev_leaf = pgno;
    for (int i = 0; i < n; i++) {
      int64_t k = leaf_key(pg, i);
      if ((has_lo && k < lo) || (has_hi && k >= hi) || (s->have_prev && k <= s->prev_key)) {
        snprintf(s->err, s->errlen, "key %lld out of order in leaf %u", (long long)k, pgno);
        return false;
      }
      s->prev_key = k;
      s->have_prev = true;
      s->count++;
    }
    return true;
  }
  if (node_type(pg) != INTERNAL || n == 0) {
    snprintf(s->err, s->errlen, "page %u is not a valid node", pgno);
    return false;
  }
  for (int i = 0; i <= n; i++) {
    int64_t clo = i == 0 ? lo : int_key(pg, i - 1);
    int64_t chi = i == n ? hi : int_key(pg, i);
    if (!check_rec(t, int_child(pg, i), clo, chi, i > 0 || has_lo, i < n || has_hi, depth + 1, s))
      return false;
  }
  return true;
}

bool btree_check(BTree *t, long *count, char *err, size_t errlen) {
  CheckState s = {0};
  s.err = err;
  s.errlen = errlen;
  bool ok = check_rec(t, t->root, 0, 0, false, false, 1, &s);
  *count = s.count;
  return ok;
}

// Small helpers: an arena allocator, CRC32 checksums, and fatal errors.

#include "jdb.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

struct ArenaBlock {
  ArenaBlock *next;
  size_t used;
  size_t size;
  _Alignas(16) unsigned char data[];
};

void *arena_alloc(Arena *a, size_t n) {
  n = (n + 15) & ~(size_t)15;
  if (!a->head || a->head->used + n > a->head->size) {
    size_t size = n > 32 * 1024 ? n : 32 * 1024;
    ArenaBlock *b = malloc(sizeof(ArenaBlock) + size);
    if (!b)
      die("out of memory");
    b->next = a->head;
    b->used = 0;
    b->size = size;
    a->head = b;
  }
  void *p = a->head->data + a->head->used;
  a->head->used += n;
  return memset(p, 0, n);
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
  char *p = arena_alloc(a, n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

char *arena_printf(Arena *a, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char *buf = arena_alloc(a, (size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(buf, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return buf;
}

void arena_free(Arena *a) {
  while (a->head) {
    ArenaBlock *next = a->head->next;
    free(a->head);
    a->head = next;
  }
}

// CRC32 (the same polynomial as zip and Ethernet). Detects torn or corrupted WAL frames:
// if even one bit of a frame changed, its stored checksum no longer matches.
uint32_t crc32(uint32_t crc, const void *data, size_t n) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++)
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  const uint8_t *p = data;
  crc = ~crc;
  while (n--)
    crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "jdb: fatal: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(2);
}

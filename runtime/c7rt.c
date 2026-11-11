/* C7109 bootstrap runtime (stage 3). Replaced by the C7-written `c7rt` in stage 4.
 * Provides only what generated code and test programs need. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void c7_print_i64(long v) { printf("%ld", v); }
void c7_print_u64(unsigned long v) { printf("%lu", v); }
void c7_print_f64(double v) { printf("%g", v); }
void c7_print_char(int cp) {  /* UTF-8 encode one code point */
  unsigned char b[4]; int n = 0;
  if (cp < 0x80) b[n++] = (unsigned char)cp;
  else if (cp < 0x800) { b[n++] = 0xC0 | (cp >> 6); b[n++] = 0x80 | (cp & 0x3F); }
  else if (cp < 0x10000) { b[n++] = 0xE0 | (cp >> 12); b[n++] = 0x80 | ((cp >> 6) & 0x3F); b[n++] = 0x80 | (cp & 0x3F); }
  else { b[n++] = 0xF0 | (cp >> 18); b[n++] = 0x80 | ((cp >> 12) & 0x3F); b[n++] = 0x80 | ((cp >> 6) & 0x3F); b[n++] = 0x80 | (cp & 0x3F); }
  fwrite(b, 1, n, stdout);
}
void c7_print_str(const char* s) { fputs(s, stdout); }
void c7_write(const unsigned char* p, unsigned long n) { fwrite(p, 1, n, stdout); }
void c7_newline(void) { fputc('\n', stdout); }
void c7_panic(const char* msg) { fflush(stdout); fprintf(stderr, "panic: %s\n", msg); abort(); }

/* Generic type descriptors (spec 10.1): { i64 size, i64 align, ptr drop, i64 n, { i64 iface_id, ptr vtable }[n] } */
struct c7_iface_entry { long id; void** vtable; };
struct c7_desc { long size, align; void* drop; long n; struct c7_iface_entry entries[]; };
void** c7_find_vtable(const struct c7_desc* d, long id) {
  for (long i = 0; i < d->n; i++) if (d->entries[i].id == id) return d->entries[i].vtable;
  c7_panic("type does not implement the required interface");
  return 0;
}

void c7_panic_str(const unsigned char* p, unsigned long n) {
  fflush(stdout);
  fputs("panic: ", stderr);
  fwrite(p, 1, n, stderr);
  fputc('\n', stderr);
  abort();
}

/* ---- debug allocator (-fdebug-alloc) ----------------------------------------------------------------------
 * Every block gets a 48-byte header (keeps 16-byte alignment) linked into a list of live blocks.
 *   double free / free of a foreign pointer -> panic (abort) at the faulty free
 *   blocks still live at exit               -> report on stderr, exit status 70
 * Freed blocks are poisoned and held in a quarantine ring so a later double free is still recognised. */
#define DBG_ALIVE 0xC7A11C0DEC7A11C0ULL
#define DBG_FREED 0xC7F4EEDC7F4EED00ULL
#define DBG_QUAR 4096
struct dbg_hdr { struct dbg_hdr *prev, *next; size_t size; uint64_t id, magic, pad; };
static struct dbg_hdr* dbg_head;
static uint64_t dbg_next_id, dbg_live, dbg_live_bytes;
static struct dbg_hdr* dbg_quar[DBG_QUAR];
static unsigned dbg_qpos;

static void dbg_report(void) {
  if (!dbg_live) return;
  fflush(stdout);
  fprintf(stderr, "c7 debug allocator: %llu block(s) leaked, %llu byte(s)\n", (unsigned long long)dbg_live, (unsigned long long)dbg_live_bytes);
  int shown = 0;
  for (struct dbg_hdr* h = dbg_head; h && shown < 20; h = h->next, shown++)
    fprintf(stderr, "  leaked block #%llu: %zu byte(s) at %p\n", (unsigned long long)h->id, h->size, (void*)(h + 1));
  _exit(70);
}

void* c7_dbg_malloc(unsigned long n) {
  static int registered;
  if (!registered) { registered = 1; atexit(dbg_report); }
  struct dbg_hdr* h = malloc(sizeof(struct dbg_hdr) + n);
  if (!h) return NULL;
  h->size = n; h->id = ++dbg_next_id; h->magic = DBG_ALIVE;
  h->prev = NULL; h->next = dbg_head;
  if (dbg_head) dbg_head->prev = h;
  dbg_head = h;
  dbg_live++; dbg_live_bytes += n;
  return h + 1;
}

void* c7_dbg_calloc(unsigned long n, unsigned long sz) {
  if (n != 0 && sz > (size_t)-1 / n) return NULL;
  void* p = c7_dbg_malloc(n * sz);
  if (p) memset(p, 0, n * sz);
  return p;
}

void c7_dbg_free(void* p) {
  if (!p) return;
  struct dbg_hdr* h = (struct dbg_hdr*)p - 1;
  if (h->magic == DBG_FREED) c7_panic("double free detected");
  if (h->magic != DBG_ALIVE) c7_panic("invalid free: pointer was not allocated by the C7 allocator");
  if (h->prev) h->prev->next = h->next; else dbg_head = h->next;
  if (h->next) h->next->prev = h->prev;
  dbg_live--; dbg_live_bytes -= h->size;
  h->magic = DBG_FREED;
  memset(p, 0xDD, h->size);
  struct dbg_hdr* old = dbg_quar[dbg_qpos];
  dbg_quar[dbg_qpos] = h;
  dbg_qpos = (dbg_qpos + 1) % DBG_QUAR;
  if (old) free(old);
}

/* ---- `c7c test` support: index of the @test function this process should run (-1 = none) ---- */
long c7_test_index(void) { const char* e = getenv("C7_TEST_INDEX"); return e ? atol(e) : -1; }

/* C7109 runtime for x86-64 Windows (stage 7). Freestanding: no C runtime, no Windows SDK headers.
 * Needs only kernel32.dll and (for %g formatting of f64) ucrtbase.dll's __stdio_common_vsprintf (Windows 10+).
 * `c7c build --target=x86_64-windows` compiles this with `clang --target=x86_64-pc-windows-msvc -ffreestanding`
 * and links it with the program using lld-link; the import libraries are generated from the .def text in the driver.
 * Behaviour mirrors runtime/c7rt.c (Linux): same function names, panic exit status 134, leak exit status 70. */
typedef unsigned long long u64;
typedef long long i64;
typedef unsigned long DWORD;
typedef unsigned long long SIZE_T;
typedef void* HANDLE;

__declspec(dllimport) HANDLE GetStdHandle(DWORD);
__declspec(dllimport) int WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
__declspec(dllimport) __declspec(noreturn) void ExitProcess(unsigned);
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void* HeapAlloc(HANDLE, DWORD, SIZE_T);
__declspec(dllimport) int HeapFree(HANDLE, DWORD, void*);
__declspec(dllimport) DWORD GetEnvironmentVariableA(const char*, char*, DWORD);
__declspec(dllimport) int SetConsoleOutputCP(unsigned);
__declspec(dllimport) int __stdio_common_vsprintf(u64 options, char* buf, SIZE_T n, const char* fmt, void* locale, __builtin_va_list args);

int _fltused = 1; /* the MSVC ABI wants this symbol when floating point is used */

/* ---- memory/string primitives LLVM may emit calls to ---- */
void* memcpy(void* d, const void* s, SIZE_T n) { void* r = d; __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory"); return r; }
void* memset(void* d, int c, SIZE_T n) { void* r = d; __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); return r; }
void* memmove(void* d, const void* s, SIZE_T n) {
  unsigned char* dd = d; const unsigned char* ss = s;
  if (dd == ss || n == 0) return d;
  if (dd < ss || dd >= ss + n) return memcpy(d, s, n);
  while (n--) dd[n] = ss[n];
  return d;
}
int memcmp(const void* a, const void* b, SIZE_T n) {
  const unsigned char *x = a, *y = b;
  for (SIZE_T i = 0; i < n; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}
static SIZE_T cstrlen(const char* s) { SIZE_T n = 0; while (s[n]) n++; return n; }

/* Stack probe: the x86-64 Windows ABI calls __chkstk (size in rax) before frames of 4 KiB or more. */
__asm__(
  ".globl __chkstk\n"
  "__chkstk:\n"
  "  pushq %rcx\n  pushq %rax\n"
  "  cmpq $0x1000, %rax\n"
  "  leaq 24(%rsp), %rcx\n"
  "  jb 2f\n"
  "1:\n"
  "  subq $0x1000, %rcx\n  testq %rcx, (%rcx)\n  subq $0x1000, %rax\n  cmpq $0x1000, %rax\n  ja 1b\n"
  "2:\n"
  "  subq %rax, %rcx\n  testq %rcx, (%rcx)\n"
  "  popq %rax\n  popq %rcx\n  ret\n");

/* ---- heap (HeapAlloc returns 16-byte aligned blocks on x64) ---- */
void* malloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
void free(void* p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
void* calloc(SIZE_T n, SIZE_T sz) {
  if (n != 0 && sz > (SIZE_T)-1 / n) return 0;
  return HeapAlloc(GetProcessHeap(), 8 /* HEAP_ZERO_MEMORY */, n * sz ? n * sz : 1);
}

/* ---- output: stdout is buffered (flushed at exit/panic), stderr is not ---- */
static char out_buf[4096];
static unsigned out_len;
static void write_fd(int fd, const void* p, SIZE_T n) {
  HANDLE h = GetStdHandle(fd == 1 ? (DWORD)-11 : (DWORD)-12);
  const char* c = p;
  while (n > 0) {
    DWORD chunk = n > 0x40000000 ? 0x40000000 : (DWORD)n, w = 0;
    if (!WriteFile(h, c, chunk, &w, 0) || w == 0) return;
    c += w; n -= w;
  }
}
static void flush_out(void) { if (out_len) { write_fd(1, out_buf, out_len); out_len = 0; } }
void c7_write(const unsigned char* p, unsigned long n) {
  SIZE_T len = n;
  if (len >= sizeof out_buf) { flush_out(); write_fd(1, p, len); return; }
  if (out_len + len > sizeof out_buf) flush_out();
  for (SIZE_T i = 0; i < len; i++) out_buf[out_len++] = (char)p[i];
}
static void err_str(const char* s) { write_fd(2, s, cstrlen(s)); }
static unsigned fmt_u64(char* b, u64 v) {
  char t[24]; unsigned n = 0, k = 0;
  do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (n) b[k++] = t[--n];
  return k;
}
static unsigned fmt_hex(char* b, u64 v) {
  static const char dg[] = "0123456789abcdef";
  char t[20]; unsigned n = 0, k = 0;
  do { t[n++] = dg[v & 15]; v >>= 4; } while (v);
  b[k++] = '0'; b[k++] = 'x';
  while (n) b[k++] = t[--n];
  return k;
}
static void err_u64(u64 v) { char b[24]; unsigned n = fmt_u64(b, v); write_fd(2, b, n); }

void c7_print_i64(long long v) {
  char b[24]; unsigned n = 0;
  if (v < 0) { b[n++] = '-'; n += fmt_u64(b + n, 0 - (u64)v); } else n = fmt_u64(b, (u64)v);
  c7_write((const unsigned char*)b, n);
}
void c7_print_u64(unsigned long long v) { char b[24]; unsigned n = fmt_u64(b, v); c7_write((const unsigned char*)b, n); }
static int fmt_g(char* buf, SIZE_T cap, double v, ...) {  /* snprintf("%g", v) through the Universal CRT */
  __builtin_va_list ap;
  __builtin_va_start(ap, v);
  int r = __stdio_common_vsprintf(2 /* standard snprintf behaviour */, buf, cap, "%g", 0, ap);
  __builtin_va_end(ap);
  return r;
}
void c7_print_f64(double v) {
  char b[64];
  /* the variadic helper receives the value as its first fixed argument; pass it again as the variadic one for %g */
  int n = fmt_g(b, sizeof b, v, v);
  if (n > 0) c7_write((const unsigned char*)b, (unsigned long)n);
}
void c7_print_char(int cp) { /* UTF-8 encode one code point */
  unsigned char b[4]; int n = 0;
  if (cp < 0x80) b[n++] = (unsigned char)cp;
  else if (cp < 0x800) { b[n++] = 0xC0 | (cp >> 6); b[n++] = 0x80 | (cp & 0x3F); }
  else if (cp < 0x10000) { b[n++] = 0xE0 | (cp >> 12); b[n++] = 0x80 | ((cp >> 6) & 0x3F); b[n++] = 0x80 | (cp & 0x3F); }
  else { b[n++] = 0xF0 | (cp >> 18); b[n++] = 0x80 | ((cp >> 12) & 0x3F); b[n++] = 0x80 | ((cp >> 6) & 0x3F); b[n++] = 0x80 | (cp & 0x3F); }
  c7_write(b, (unsigned long)n);
}
void c7_print_str(const char* s) { c7_write((const unsigned char*)s, (unsigned long)cstrlen(s)); }
void c7_newline(void) { unsigned char c = '\n'; c7_write(&c, 1); }

__declspec(noreturn) void c7_panic(const char* msg) {
  flush_out();
  err_str("panic: "); err_str(msg); err_str("\n");
  ExitProcess(134); /* same status as abort() under the Linux runtime */
}
__declspec(noreturn) void c7_panic_str(const unsigned char* p, unsigned long n) {
  flush_out();
  err_str("panic: "); write_fd(2, p, n); err_str("\n");
  ExitProcess(134);
}

/* Generic type descriptors (spec 10.1): { i64 size, i64 align, ptr drop, i64 n, { i64 iface_id, ptr vtable }[n] } */
struct c7_iface_entry { long long id; void** vtable; };
struct c7_desc { long long size, align; void* drop; long long n; struct c7_iface_entry entries[]; };
void** c7_find_vtable(const struct c7_desc* d, long long id) {
  for (long long i = 0; i < d->n; i++) if (d->entries[i].id == id) return d->entries[i].vtable;
  c7_panic("type does not implement the required interface");
  return 0;
}

/* ---- debug allocator (-fdebug-alloc): same design as the Linux runtime ---- */
#define DBG_ALIVE 0xC7A11C0DEC7A11C0ULL
#define DBG_FREED 0xC7F4EEDC7F4EED00ULL
#define DBG_QUAR 4096
struct dbg_hdr { struct dbg_hdr *prev, *next; SIZE_T size; u64 id, magic, pad; };
static struct dbg_hdr* dbg_head;
static u64 dbg_next_id, dbg_live, dbg_live_bytes;
static struct dbg_hdr* dbg_quar[DBG_QUAR];
static unsigned dbg_qpos;

static void dbg_report(void) {
  if (!dbg_live) return;
  flush_out();
  err_str("c7 debug allocator: "); err_u64(dbg_live); err_str(" block(s) leaked, "); err_u64(dbg_live_bytes); err_str(" byte(s)\n");
  int shown = 0;
  for (struct dbg_hdr* h = dbg_head; h && shown < 20; h = h->next, shown++) {
    char hx[24]; unsigned k = fmt_hex(hx, (u64)(h + 1)); hx[k] = 0;
    err_str("  leaked block #"); err_u64(h->id); err_str(": "); err_u64(h->size); err_str(" byte(s) at "); err_str(hx); err_str("\n");
  }
  ExitProcess(70);
}
void* c7_dbg_malloc(unsigned long n) {
  struct dbg_hdr* h = malloc(sizeof(struct dbg_hdr) + n);
  if (!h) return 0;
  h->size = n; h->id = ++dbg_next_id; h->magic = DBG_ALIVE;
  h->prev = 0; h->next = dbg_head;
  if (dbg_head) dbg_head->prev = h;
  dbg_head = h;
  dbg_live++; dbg_live_bytes += n;
  return h + 1;
}
void* c7_dbg_calloc(unsigned long n, unsigned long sz) {
  if (n != 0 && sz > (SIZE_T)-1 / n) return 0;
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

/* ---- `c7c test` support ---- */
long long c7_test_index(void) {
  char b[32];
  DWORD n = GetEnvironmentVariableA("C7_TEST_INDEX", b, sizeof b);
  if (n == 0 || n >= sizeof b) return -1;
  long long v = 0;
  for (DWORD i = 0; i < n; i++) { if (b[i] < '0' || b[i] > '9') return -1; v = v * 10 + (b[i] - '0'); }
  return v;
}

/* ---- process entry (lld-link /entry:c7_start) ---- */
extern int main(void);
__declspec(noreturn) void c7_start(void) {
  SetConsoleOutputCP(65001); /* programs write UTF-8 */
  int rc = main();
  flush_out();
  dbg_report(); /* exits with 70 when blocks leaked */
  ExitProcess((unsigned)rc);
}

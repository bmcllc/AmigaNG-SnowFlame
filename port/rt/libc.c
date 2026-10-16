/* libc.c - libc mínima do runtime SF-100: strings, memória, malloc, qsort, rand, printf (para o console de depuração). */
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "sf_runtime.h"

/* ------------------------------ memória e strings ------------------------------ */
void *memcpy(void *d, const void *s, size_t n) {
  unsigned char *dd = d; const unsigned char *ss = s;
  if (((uintptr_t)d | (uintptr_t)s | n) % 4 == 0) { uint32_t *dw = d; const uint32_t *sw = s; n /= 4; while (n--) *dw++ = *sw++; return d; }
  while (n--) *dd++ = *ss++;
  return d;
}
void *memmove(void *d, const void *s, size_t n) {
  unsigned char *dd = d; const unsigned char *ss = s;
  if (dd == ss || n == 0) return d;
  if (dd < ss) while (n--) *dd++ = *ss++;
  else { dd += n; ss += n; while (n--) *--dd = *--ss; }
  return d;
}
void *memset(void *d, int c, size_t n) {
  unsigned char *p = d; unsigned char v = (unsigned char)c;
  if (((uintptr_t)d | n) % 4 == 0) { uint32_t w = v * 0x01010101u, *pw = d; n /= 4; while (n--) *pw++ = w; return d; }
  while (n--) *p++ = v;
  return d;
}
int memcmp(const void *a, const void *b, size_t n) { const unsigned char *x = a, *y = b; while (n--) { if (*x != *y) return *x - *y; x++; y++; } return 0; }
void *memchr(const void *s, int c, size_t n) { const unsigned char *p = s; while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; } return 0; }
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
size_t strnlen(const char *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) { } return r; }
char *strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strncat(char *d, const char *s, size_t n) { char *e = d + strlen(d); size_t i = 0; for (; i < n && s[i]; i++) e[i] = s[i]; e[i] = 0; return d; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? (unsigned char)*a - (unsigned char)*b : 0; }
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; } }
char *strrchr(const char *s, int c) { const char *r = 0; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
char *strstr(const char *h, const char *n) { size_t l = strlen(n); if (!l) return (char *)h; for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h; return 0; }

/* ------------------------------ malloc (primeiro-que-couber, com fusão) ------------------------------ */
struct blk { size_t size; int used; struct blk *next; };
#define HEAP_LIMIT 0x03C00000u                                    /* até 60 MB (a DDR tem 64) */
static struct blk *heap_head;
void __rt_init(void) {
  uintptr_t s = ((uintptr_t)_end + 15) & ~(uintptr_t)15;
  heap_head = (struct blk *)s; heap_head->size = HEAP_LIMIT - s - sizeof(struct blk); heap_head->used = 0; heap_head->next = 0;
}
void *malloc(size_t n) {
  struct blk *b;
  n = (n + 15) & ~(size_t)15; if (!n) n = 16;
  for (b = heap_head; b; b = b->next) {
    if (b->used || b->size < n) continue;
    if (b->size >= n + sizeof(struct blk) + 32) {
      struct blk *r = (struct blk *)((char *)(b + 1) + n);
      r->size = b->size - n - sizeof(struct blk); r->used = 0; r->next = b->next; b->size = n; b->next = r;
    }
    b->used = 1; return b + 1;
  }
  return 0;
}
void free(void *p) {
  struct blk *b, *n;
  if (!p) return;
  ((struct blk *)p - 1)->used = 0;
  for (b = heap_head; b; b = b->next) while (!b->used && b->next && !b->next->used) { n = b->next; b->size += sizeof(struct blk) + n->size; b->next = n->next; }
}
/* diagnóstico: maior bloco livre e total livre / usado (sf_malloc_stats) */
void sf_malloc_stats(size_t *largest, size_t *freeb, size_t *usedb) {
  struct blk *b; *largest = *freeb = *usedb = 0;
  for (b = heap_head; b; b = b->next) { if (b->used) *usedb += b->size; else { *freeb += b->size; if (b->size > *largest) *largest = b->size; } }
}
void *calloc(size_t n, size_t m) { size_t t = n * m; void *p = malloc(t); if (p) memset(p, 0, t); return p; }
void *realloc(void *p, size_t n) {
  struct blk *b; void *q;
  if (!p) return malloc(n);
  b = (struct blk *)p - 1; if (b->size >= n) return p;
  q = malloc(n); if (!q) return 0;
  memcpy(q, p, b->size); free(p); return q;
}

/* ------------------------------ utilidades ------------------------------ */
void exit(int code) { register int d0 __asm__("d0") = code; __asm__ volatile("move.l %0,__exit_code\n\thalt" : : "d"(d0)); for (;;) { } }
void abort(void) { printf("abort()\n"); exit(134); }
int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
static unsigned rng = 1;
void srand(unsigned s) { rng = s; }
int rand(void) { rng = rng * 1103515245u + 12345u; return (int)((rng >> 1) & 0x7FFFFFFF); }
long strtol(const char *s, char **end, int base) {
  long v = 0; int neg = 0;
  while (*s == ' ' || *s == '\t' || *s == '\n') s++;
  if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
  if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; } else if (base == 0) base = s[0] == '0' ? 8 : 10;
  for (;; s++) { int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'z' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'Z' ? *s - 'A' + 10 : 99; if (d >= base) break; v = v * base + d; }
  if (end) *end = (char *)s;
  return neg ? -v : v;
}
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
double atof(const char *s) {
  double v = 0, f = 0.1; int neg = 0, e = 0;
  while (*s == ' ') s++;
  if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { v += (*s++ - '0') * f; f /= 10; } }
  if (*s == 'e' || *s == 'E') { s++; e = (int)strtol(s, 0, 10); v *= pow(10.0, e); }
  return neg ? -v : v;
}
static void swapb(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {   /* quicksort com inserção para partições pequenas */
  char *a = base;
  while (n > 12) {
    size_t i = 0, j = n - 1; char *pv = a + (n / 2) * sz;
    swapb(pv, a + (n - 1) * sz, sz); pv = a + (n - 1) * sz;
    for (i = 0, j = 0; i < n - 1; i++) if (cmp(a + i * sz, pv) < 0) { swapb(a + i * sz, a + j * sz, sz); j++; }
    swapb(a + j * sz, pv, sz);
    if (j < n - j - 1) { qsort(a, j, sz, cmp); a += (j + 1) * sz; n -= j + 1; } else { qsort(a + (j + 1) * sz, n - j - 1, sz, cmp); n = j; }
  }
  { size_t i, j; for (i = 1; i < n; i++) for (j = i; j > 0 && cmp(a + (j - 1) * sz, a + j * sz) > 0; j--) swapb(a + (j - 1) * sz, a + j * sz, sz); }
}

/* ------------------------------ printf ------------------------------ */
struct out { char *buf; size_t cap, n; int cons; };                  /* cons: escreve no console; senão grava em buf (buf NULL = só conta) */
static void oc(struct out *o, char c) { if (o->cons) SF_DBG_PUTC = (unsigned char)c; else if (o->buf && o->n + 1 < o->cap) o->buf[o->n] = c; o->n++; }
static void pad(struct out *o, int n, char c) { while (n-- > 0) oc(o, c); }
static void fmt_uint(struct out *o, unsigned long long v, int base, int upper, int width, int zero, int left, int prec, const char *sign) {
  char tmp[72]; int k = 0, len, sl = (int)strlen(sign); const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  if (v == 0 && prec != 0) tmp[k++] = '0';
  while (v) { unsigned d = (unsigned)(base == 10 ? v % 10 : v % (unsigned)base); tmp[k++] = dig[d]; v = base == 10 ? v / 10 : v / (unsigned)base; }
  len = k > prec ? k : prec;
  if (!left && !zero) pad(o, width - len - sl, ' ');
  { int i; for (i = 0; i < sl; i++) oc(o, sign[i]); }
  if (!left && zero && prec < 0) pad(o, width - len - sl, '0');
  pad(o, len - k, '0');
  while (k) oc(o, tmp[--k]);
  if (left) pad(o, width - len - sl, ' ');
}
static void fmt_double(struct out *o, double v, char conv, int width, int zero, int left, int prec, int plus, int space) {
  char tmp[400]; int k = 0, neg = 0, i; const char *sign = "";
  if (v != v) { pad(o, width - 3, ' '); oc(o, 'n'); oc(o, 'a'); oc(o, 'n'); return; }
  if (v < 0 || (v == 0 && 1.0 / v < 0)) { neg = 1; v = -v; }
  sign = neg ? "-" : plus ? "+" : space ? " " : "";
  if (v > 1e300) { pad(o, width - 3 - (int)strlen(sign), ' '); for (i = 0; sign[i]; i++) oc(o, sign[i]); oc(o, 'i'); oc(o, 'n'); oc(o, 'f'); return; }
  if (prec < 0) prec = 6;
  if (conv == 'g') {                                              /* %g simples: 6 algarismos significativos, zeros finais removidos */
    int e10 = 0; double t = v; int sig = prec ? prec : 1;
    if (t != 0) { while (t >= 10) { t /= 10; e10++; } while (t < 1) { t *= 10; e10--; } }
    if (e10 < -4 || e10 >= sig) { conv = 'e'; prec = sig - 1; } else { conv = 'f'; prec = sig - 1 - e10; if (prec < 0) prec = 0; }
    { char tb[400]; struct out o2 = { tb, sizeof tb, 0 }; fmt_double(&o2, neg ? -v : v, conv, 0, 0, 0, prec, 0, 0); tb[o2.n < sizeof tb ? o2.n : sizeof tb - 1] = 0;
      { char *dot = strchr(tb, '.'); char *e = strchr(tb, 'e'); if (dot) { char *end = e ? e : tb + strlen(tb); char *p = end; while (p > dot + 1 && p[-1] == '0') p--; if (p == dot + 1) p--; memmove(p, end, strlen(end) + 1); } }
      { int l = (int)strlen(tb); if (!left) pad(o, width - l, zero ? '0' : ' '); for (i = 0; i < l; i++) oc(o, tb[i]); if (left) pad(o, width - l, ' '); } return; }
  }
  if (conv == 'e') {
    int e10 = 0; double t = v; unsigned long long ip; double scale = 1;
    if (t != 0) { while (t >= 10) { t /= 10; e10++; } while (t < 1) { t *= 10; e10--; } }
    for (i = 0; i < prec; i++) scale *= 10;
    ip = (unsigned long long)(t * scale + 0.5); if (ip >= (unsigned long long)(scale * 10)) { ip /= 10; e10++; }
    { char d[40]; int nd = 0; unsigned long long x = ip; for (i = 0; i <= prec; i++) { d[nd++] = (char)('0' + (int)(x % 10)); x /= 10; }
      for (i = nd - 1; i >= 0; i--) { tmp[k++] = d[i]; if (i == nd - 1 && prec > 0) tmp[k++] = '.'; } }
    tmp[k++] = 'e'; tmp[k++] = e10 < 0 ? '-' : '+'; { int ae = e10 < 0 ? -e10 : e10; tmp[k++] = (char)('0' + ae / 10); tmp[k++] = (char)('0' + ae % 10); }
  } else {
    double scale = 1, r; unsigned long long ip, fp;
    for (i = 0; i < prec; i++) scale *= 10;
    r = v * scale; if (r >= 1.8e19) { ip = (unsigned long long)v; fp = 0; } else { unsigned long long all = (unsigned long long)r; double rem = r - (double)all; if (rem > 0.5 || (rem == 0.5 && (all & 1))) all++; ip = all / (unsigned long long)scale; fp = all % (unsigned long long)scale; }   /* arredonda para par nos empates, como o glibc */
    { char d[40]; int nd = 0; if (ip == 0) d[nd++] = '0'; while (ip) { d[nd++] = (char)('0' + (int)(ip % 10)); ip /= 10; } while (nd) tmp[k++] = d[--nd]; }
    if (prec > 0) { tmp[k++] = '.'; for (i = prec - 1; i >= 0; i--) { unsigned long long p = 1; int j; for (j = 0; j < i; j++) p *= 10; tmp[k++] = (char)('0' + (int)((fp / p) % 10)); } }
  }
  { int sl = (int)strlen(sign); if (!left && !zero) pad(o, width - k - sl, ' '); for (i = 0; i < sl; i++) oc(o, sign[i]); if (!left && zero) pad(o, width - k - sl, '0'); for (i = 0; i < k; i++) oc(o, tmp[i]); if (left) pad(o, width - k - sl, ' '); }
}
static int vfmt(struct out *o, const char *f, va_list ap) {
  for (; *f; f++) {
    int left = 0, plus = 0, space = 0, zero = 0, alt = 0, width = 0, prec = -1, lng = 0; char c;
    if (*f != '%') { oc(o, *f); continue; }
    f++;
    for (;; f++) { if (*f == '-') left = 1; else if (*f == '+') plus = 1; else if (*f == ' ') space = 1; else if (*f == '0') zero = 1; else if (*f == '#') alt = 1; else break; }
    if (*f == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } f++; } else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = va_arg(ap, int); f++; } else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
    while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 't' || *f == 'j') { if (*f == 'l' || *f == 'z' || *f == 'j') lng++; f++; }
    c = *f; if (!c) break;
    if (c == 'd' || c == 'i') { long long v = lng >= 2 ? va_arg(ap, long long) : va_arg(ap, int); const char *sg = v < 0 ? "-" : plus ? "+" : space ? " " : ""; fmt_uint(o, v < 0 ? (unsigned long long)-v : (unsigned long long)v, 10, 0, width, zero, left, prec, sg); }
    else if (c == 'u' || c == 'x' || c == 'X' || c == 'o') { unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned); fmt_uint(o, v, c == 'u' ? 10 : c == 'o' ? 8 : 16, c == 'X', width, zero, left, prec, alt && v && c != 'u' ? (c == 'o' ? "0" : c == 'X' ? "0X" : "0x") : ""); }
    else if (c == 'p') { fmt_uint(o, (unsigned long long)(uintptr_t)va_arg(ap, void *), 16, 0, width, zero, left, prec, "0x"); }
    else if (c == 'c') { if (!left) pad(o, width - 1, ' '); oc(o, (char)va_arg(ap, int)); if (left) pad(o, width - 1, ' '); }
    else if (c == 's') { const char *s = va_arg(ap, const char *); int l, i; if (!s) s = "(null)"; l = prec >= 0 ? (int)strnlen(s, (size_t)prec) : (int)strlen(s); if (!left) pad(o, width - l, ' '); for (i = 0; i < l; i++) oc(o, s[i]); if (left) pad(o, width - l, ' '); }
    else if (c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G') fmt_double(o, va_arg(ap, double), (char)(c | 0x20), width, zero, left, prec, plus, space);
    else if (c == '%') oc(o, '%');
    else { oc(o, '%'); oc(o, c); }
  }
  if (o->buf && o->cap) o->buf[o->n < o->cap ? o->n : o->cap - 1] = 0;
  return (int)o->n;
}
int vsnprintf(char *b, size_t n, const char *f, va_list ap) { struct out o = { b, n, 0, 0 }; return vfmt(&o, f, ap); }
int vsprintf(char *b, const char *f, va_list ap) { struct out o = { b, (size_t)-1, 0, 0 }; return vfmt(&o, f, ap); }
int vprintf(const char *f, va_list ap) { struct out o = { 0, 0, 0, 1 }; return vfmt(&o, f, ap); }
int printf(const char *f, ...) { va_list ap; int r; va_start(ap, f); r = vprintf(f, ap); va_end(ap); return r; }
int sprintf(char *b, const char *f, ...) { va_list ap; int r; va_start(ap, f); r = vsprintf(b, f, ap); va_end(ap); return r; }
int snprintf(char *b, size_t n, const char *f, ...) { va_list ap; int r; va_start(ap, f); r = vsnprintf(b, n, f, ap); va_end(ap); return r; }
int putchar(int c) { SF_DBG_PUTC = (unsigned char)c; return c; }
int puts(const char *s) { while (*s) SF_DBG_PUTC = (unsigned char)*s++; SF_DBG_PUTC = '\n'; return 0; }

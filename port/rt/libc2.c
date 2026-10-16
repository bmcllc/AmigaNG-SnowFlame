/* libc2.c - libc ampliada: ctype, errno, strings extras, time, strtod, sscanf, stdio (arquivos do DVD pelo SnowFS + arquivos em RAM), math extra. */
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <locale.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <assert.h>
#include "sf_runtime.h"
#include "sf_fs.h"

int errno;
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isgraph(int c) { return c > 32 && c < 127; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isprint(int c) { return c >= 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isascii(int c) { return c >= 0 && c < 128; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int toupper(int c) { return islower(c) ? c - 32 : c; }

int strcasecmp(const char *a, const char *b) { while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; } return tolower((unsigned char)*a) - tolower((unsigned char)*b); }
int strncasecmp(const char *a, const char *b, size_t n) { while (n && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; n--; } return n ? tolower((unsigned char)*a) - tolower((unsigned char)*b) : 0; }
char *strdup(const char *s) { size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }
size_t strspn(const char *s, const char *set) { size_t n = 0; while (s[n] && strchr(set, s[n])) n++; return n; }
size_t strcspn(const char *s, const char *set) { size_t n = 0; while (s[n] && !strchr(set, s[n])) n++; return n; }
char *strpbrk(const char *s, const char *set) { for (; *s; s++) if (strchr(set, *s)) return (char *)s; return 0; }
char *strtok(char *s, const char *d) { static char *save; char *r; if (!s) s = save; if (!s) return 0; s += strspn(s, d); if (!*s) { save = 0; return 0; } r = s; s += strcspn(s, d); if (*s) { *s++ = 0; save = s; } else save = 0; return r; }
char *strerror(int e) { switch (e) { case ENOENT: return "No such file or directory"; case ENOMEM: return "Out of memory"; case EINVAL: return "Invalid argument"; case EACCES: return "Permission denied"; case EIO: return "I/O error"; default: return "Error"; } }
char *getenv(const char *n) { (void)n; return 0; }
int system(const char *c) { (void)c; return -1; }
int atexit(void (*f)(void)) { (void)f; return 0; }
sighandler_t signal(int s, sighandler_t h) { (void)s; (void)h; return SIG_DFL; }
char *setlocale(int c, const char *l) { (void)c; (void)l; return "C"; }
void __assert_fail(const char *e, const char *f, int l) { printf("assert falhou: %s (%s:%d)\n", e, f, l); abort(); }
void *bsearch(const void *k, const void *b, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
  size_t lo = 0, hi = n;
  while (lo < hi) { size_t m = (lo + hi) / 2; const void *p = (const char *)b + m * sz; int c = cmp(k, p); if (c == 0) return (void *)p; if (c < 0) hi = m; else lo = m + 1; }
  return 0;
}
long atol(const char *s) { return strtol(s, 0, 10); }
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtol(s, end, base); }
long long strtoll(const char *s, char **end, int base) {
  long long v = 0; int neg = 0;
  while (isspace((unsigned char)*s)) s++;
  if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
  if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; } else if (base == 0) base = s[0] == '0' ? 8 : 10;
  for (;; s++) { int d = isdigit((unsigned char)*s) ? *s - '0' : isalpha((unsigned char)*s) ? tolower((unsigned char)*s) - 'a' + 10 : 99; if (d >= base) break; v = v * base + d; }
  if (end) *end = (char *)s;
  return neg ? -v : v;
}
unsigned long long strtoull(const char *s, char **end, int base) { return (unsigned long long)strtoll(s, end, base); }
double strtod(const char *s, char **end) {
  double v = 0, scale = 1; int neg = 0, any = 0, e = 0; const char *p = s;
  while (isspace((unsigned char)*p)) p++;
  if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
  if (!strncasecmp(p, "inf", 3)) { if (end) *end = (char *)(p + 3); return neg ? -HUGE_VAL : HUGE_VAL; }
  if (!strncasecmp(p, "nan", 3)) { if (end) *end = (char *)(p + 3); return __builtin_nan(""); }
  while (isdigit((unsigned char)*p)) { v = v * 10 + (*p++ - '0'); any = 1; }
  if (*p == '.') { p++; while (isdigit((unsigned char)*p)) { v = v * 10 + (*p++ - '0'); scale *= 10; any = 1; } }
  if (!any) { if (end) *end = (char *)s; return 0; }
  if (*p == 'e' || *p == 'E') { const char *q = p + 1; int en = 0; if (*q == '-') { en = 1; q++; } else if (*q == '+') q++; if (isdigit((unsigned char)*q)) { while (isdigit((unsigned char)*q)) e = e * 10 + (*q++ - '0'); if (en) e = -e; p = q; } }
  v /= scale; if (e) v *= pow(10.0, e);
  if (end) *end = (char *)p;
  return neg ? -v : v;
}
float strtof(const char *s, char **end) { return (float)strtod(s, end); }

/* ------------------------------ tempo (relógio do SnowBridge em ns) ------------------------------ */
unsigned long long sf_time_ns(void) {
  volatile unsigned *b = (volatile unsigned *)0xF0000000u; unsigned lo = b[2], hi = b[3];
  return ((unsigned long long)hi << 32) | lo;
}
#define EPOCH0 1700000000L
time_t time(time_t *t) { time_t v = EPOCH0 + (time_t)(sf_time_ns() / 1000000000ull); if (t) *t = v; return v; }
clock_t clock(void) { return (clock_t)(sf_time_ns() / 1000ull); }
int gettimeofday(struct timeval *tv, void *tz) { unsigned long long n = sf_time_ns(); (void)tz; tv->tv_sec = EPOCH0 + (long)(n / 1000000000ull); tv->tv_usec = (long)((n / 1000ull) % 1000000ull); return 0; }
double difftime(time_t a, time_t b) { return (double)(a - b); }
struct tm *gmtime(const time_t *tp) {
  static struct tm tm; long t = *tp, days = t / 86400, rem = t % 86400; long y, m, d, z, era, doe, yoe, doy, mp;
  if (rem < 0) { rem += 86400; days--; }
  tm.tm_hour = (int)(rem / 3600); tm.tm_min = (int)(rem % 3600 / 60); tm.tm_sec = (int)(rem % 60); tm.tm_wday = (int)((days + 4) % 7); if (tm.tm_wday < 0) tm.tm_wday += 7;
  z = days + 719468; era = (z >= 0 ? z : z - 146096) / 146097; doe = z - era * 146097; yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; y = yoe + era * 400;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100); mp = (5 * doy + 2) / 153; d = doy - (153 * mp + 2) / 5 + 1; m = mp < 10 ? mp + 3 : mp - 9; if (m <= 2) y++;
  tm.tm_year = (int)(y - 1900); tm.tm_mon = (int)(m - 1); tm.tm_mday = (int)d; tm.tm_isdst = 0;
  { static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334}; int leap = ((y % 4 == 0) && (y % 100 != 0)) || y % 400 == 0; tm.tm_yday = cum[m - 1] + (int)d - 1 + (leap && m > 2); }
  return &tm;
}
struct tm *localtime(const time_t *t) { return gmtime(t); }
time_t mktime(struct tm *tm) {
  long y = tm->tm_year + 1900L, m = tm->tm_mon + 1, d = tm->tm_mday, era, yoe, doy, doe;
  if (m <= 2) y--;
  era = (y >= 0 ? y : y - 399) / 400; yoe = y - era * 400; doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1; doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (era * 146097 + doe - 719468) * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
}
static const char *WD[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"}, *MO[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
char *asctime(const struct tm *tm) { static char b[32]; snprintf(b, sizeof b, "%s %s %2d %02d:%02d:%02d %d\n", WD[tm->tm_wday % 7], MO[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900); return b; }
char *ctime(const time_t *t) { return asctime(gmtime(t)); }
size_t strftime(char *s, size_t max, const char *f, const struct tm *tm) {
  size_t n = 0; char t[32];
  for (; *f && n + 1 < max; f++) {
    if (*f != '%') { s[n++] = *f; continue; }
    f++;
    switch (*f) {
      case 'Y': snprintf(t, sizeof t, "%d", tm->tm_year + 1900); break; case 'm': snprintf(t, sizeof t, "%02d", tm->tm_mon + 1); break; case 'd': snprintf(t, sizeof t, "%02d", tm->tm_mday); break;
      case 'H': snprintf(t, sizeof t, "%02d", tm->tm_hour); break; case 'M': snprintf(t, sizeof t, "%02d", tm->tm_min); break; case 'S': snprintf(t, sizeof t, "%02d", tm->tm_sec); break;
      case 'a': snprintf(t, sizeof t, "%s", WD[tm->tm_wday % 7]); break; case 'b': snprintf(t, sizeof t, "%s", MO[tm->tm_mon % 12]); break;
      case 'y': snprintf(t, sizeof t, "%02d", tm->tm_year % 100); break; case '%': snprintf(t, sizeof t, "%%"); break;
      default: snprintf(t, sizeof t, "%%%c", *f); break;
    }
    { size_t l = strlen(t); if (n + l + 1 >= max) break; memcpy(s + n, t, l); n += l; }
  }
  s[n] = 0; return n;
}

/* ------------------------------ sscanf ------------------------------ */
int vsscanf(const char *s, const char *f, va_list ap) {
  int count = 0; const char *start = s;
  for (; *f; f++) {
    int suppress = 0, width = 0, lng = 0;
    if (isspace((unsigned char)*f)) { while (isspace((unsigned char)*s)) s++; continue; }
    if (*f != '%') { if (*s != *f) return count; s++; continue; }
    f++; if (*f == '%') { if (*s != '%') return count; s++; continue; }
    if (*f == '*') { suppress = 1; f++; }
    while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
    while (*f == 'l' || *f == 'h' || *f == 'L') { if (*f == 'l' || *f == 'L') lng++; f++; }
    if (*f == 'n') { if (!suppress) *va_arg(ap, int *) = (int)(s - start); continue; }
    if (*f != 'c' && *f != '[') while (isspace((unsigned char)*s)) s++;
    if (!*s && *f != 'n') return count ? count : -1;
    if (*f == 'd' || *f == 'i' || *f == 'u' || *f == 'x' || *f == 'X' || *f == 'o') {
      char *e; long long v = strtoll(s, &e, *f == 'x' || *f == 'X' ? 16 : *f == 'o' ? 8 : *f == 'i' ? 0 : 10);
      if (e == s) return count;
      if (width && e - s > width) { char tmp[32]; int n = width < 31 ? width : 31; memcpy(tmp, s, n); tmp[n] = 0; v = strtoll(tmp, 0, *f == 'x' ? 16 : 10); e = (char *)s + n; }
      s = e; if (!suppress) { if (lng >= 2) *va_arg(ap, long long *) = v; else if (lng == 1) *va_arg(ap, long *) = (long)v; else *va_arg(ap, int *) = (int)v; count++; }
    } else if (*f == 'f' || *f == 'e' || *f == 'g' || *f == 'E') {
      char *e; double v = strtod(s, &e); if (e == s) return count; s = e;
      if (!suppress) { if (lng) *va_arg(ap, double *) = v; else *va_arg(ap, float *) = (float)v; count++; }
    } else if (*f == 's') {
      char *d = suppress ? 0 : va_arg(ap, char *); int n = 0;
      while (*s && !isspace((unsigned char)*s) && (!width || n < width)) { if (d) d[n] = *s; n++; s++; }
      if (d) d[n] = 0; if (!suppress) count++;
    } else if (*f == 'c') {
      char *d = suppress ? 0 : va_arg(ap, char *); int n = width ? width : 1; while (n-- && *s) { if (d) *d++ = *s; s++; } if (!suppress) count++;
    } else if (*f == '[') {
      char set[256]; int neg = 0, k = 0; char *d = suppress ? 0 : va_arg(ap, char *); int n = 0;
      memset(set, 0, sizeof set); f++; if (*f == '^') { neg = 1; f++; }
      if (*f == ']') { set[(unsigned char)']'] = 1; f++; }
      for (; *f && *f != ']'; f++) { if (f[1] == '-' && f[2] && f[2] != ']') { int c; for (c = (unsigned char)f[0]; c <= (unsigned char)f[2]; c++) set[c] = 1; f += 2; } else set[(unsigned char)*f] = 1; }
      (void)k;
      while (*s && (set[(unsigned char)*s] != 0) != neg && (!width || n < width)) { if (d) d[n] = *s; n++; s++; }
      if (!n) return count; if (d) d[n] = 0; if (!suppress) count++;
    } else return count;
  }
  return count;
}
int sscanf(const char *s, const char *f, ...) { va_list ap; int r; va_start(ap, f); r = vsscanf(s, f, ap); va_end(ap); return r; }

/* ------------------------------ DVD / SnowFS ------------------------------ */
#define DVD_REG(o) (*(volatile unsigned *)(0xF0090000u + (o)))
#define BR_REG(o) (*(volatile unsigned *)(0xF0000000u + (o)))
#define SECT 2048
static void cache_range(unsigned op, unsigned a, unsigned n) { BR_REG(0x24) = a; BR_REG(0x28) = n; BR_REG(0x20) = op; }
static int dvd_read(unsigned lba, unsigned n, void *dst) {
  unsigned st;
  cache_range(6, (unsigned)dst, n * SECT);                       /* flush + invalidate: a D-cache não é coerente com o DMA */
  DVD_REG(0x10) = 6; DVD_REG(0x04) = lba; DVD_REG(0x08) = n; DVD_REG(0x0C) = (unsigned)dst; DVD_REG(0x00) = 1;
  do { st = DVD_REG(0x10); if (st & 1) BR_REG(0xF4) = 100; } while (st & 1);      /* BR_SLEEP_US: espera sem queimar instruções (seek/spin-up levam centenas de ms) */
  DVD_REG(0x10) = 6;
  cache_range(5, (unsigned)dst, n * SECT);
  return (st & 4) ? -1 : 0;
}
struct dent { char name[48]; unsigned lba, size, flags, pad; };
static struct dent *dir; static int ndir, mounted;
int sf_fs_mount(void) {
  unsigned *sb; unsigned char *b; unsigned dsec;
  if (mounted) return 0;
  if (!(DVD_REG(0x10) & 8)) return -1;
  b = malloc(SECT); if (!b || dvd_read(0, 1, b)) return -1;
  sb = (unsigned *)b;
  if (sb[0] != 0x53464653u || sb[1] != 1) { free(b); return -1; }
  ndir = (int)sb[2]; dsec = sb[4]; dir = malloc(dsec * SECT);
  if (!dir || dvd_read(sb[3], dsec, dir)) { free(b); return -1; }
  free(b); mounted = 1; return 0;
}
int sf_fs_count(void) { sf_fs_mount(); return ndir; }
const char *sf_fs_name(int i) { return i >= 0 && i < ndir ? dir[i].name : 0; }
unsigned sf_fs_size(int i) { return i >= 0 && i < ndir ? dir[i].size : 0; }
int sf_fs_find(const char *p) { int i; sf_fs_mount(); while (p[0] == '.' && p[1] == '/') p += 2; while (*p == '/') p++; for (i = 0; i < ndir; i++) if (!strncmp(dir[i].name, p, 48)) return i; return -1; }

/* ------------------------------ arquivos em RAM (fopen "w") ------------------------------ */
struct ramfile { char name[128]; unsigned char *data; unsigned size, cap; int used; };
static struct ramfile rams[64];
static struct ramfile *ram_find(const char *p) { int i; while (p[0] == '.' && p[1] == '/') p += 2; while (*p == '/') p++; for (i = 0; i < 64; i++) if (rams[i].used && !strncmp(rams[i].name, p, 127)) return &rams[i]; return 0; }
static struct ramfile *ram_create(const char *p) {
  int i; struct ramfile *r = ram_find(p);
  while (p[0] == '.' && p[1] == '/') p += 2; while (*p == '/') p++;
  if (r) { r->size = 0; return r; }
  for (i = 0; i < 64; i++) if (!rams[i].used) { rams[i].used = 1; strncpy(rams[i].name, p, 127); rams[i].name[127] = 0; rams[i].size = 0; return &rams[i]; }
  return 0;
}
int sf_ram_count(void) { return 64; }
const char *sf_ram_name(int i) { return i >= 0 && i < 64 && rams[i].used ? rams[i].name : 0; }
unsigned sf_ram_size(int i) { return i >= 0 && i < 64 && rams[i].used ? rams[i].size : 0; }

/* ------------------------------ stdio ------------------------------ */
struct sf_FILE { int kind; unsigned lba, size, pos; int eof, err, writable, unget; struct ramfile *rf; unsigned char *sect; unsigned sect_lba; };
enum { K_FREE, K_DVD, K_RAM, K_OUT, K_IN };
static struct sf_FILE files[40] = { {K_IN}, {K_OUT}, {K_OUT} };
FILE *stdin = &files[0], *stdout = &files[1], *stderr = &files[2];

FILE *fopen(const char *path, const char *mode) {
  struct sf_FILE *f = 0; int i, w = mode[0] == 'w' || mode[0] == 'a' || strchr(mode, '+');
  for (i = 3; i < 40; i++) if (files[i].kind == K_FREE) { f = &files[i]; break; }
  if (!f) { errno = ENOMEM; return 0; }
  memset(f, 0, sizeof *f); f->unget = -1; f->sect_lba = 0xFFFFFFFFu;
  if (w) {
    struct ramfile *r = mode[0] == 'r' ? ram_find(path) : mode[0] == 'a' ? (ram_find(path) ? ram_find(path) : ram_create(path)) : ram_create(path);
    if (!r) { errno = mode[0] == 'r' ? ENOENT : ENOMEM; return 0; }
    f->kind = K_RAM; f->rf = r; f->writable = 1; f->pos = mode[0] == 'a' ? r->size : 0; return f;
  }
  { struct ramfile *r = ram_find(path); if (r) { f->kind = K_RAM; f->rf = r; return f; } }
  i = sf_fs_find(path);
  if (i < 0) { errno = ENOENT; return 0; }
  f->kind = K_DVD; f->lba = dir[i].lba; f->size = dir[i].size;
  return f;
}
int fclose(FILE *f) { if (!f || f->kind == K_FREE) return EOF; if (f->sect) free(f->sect); if (f >= &files[3]) f->kind = K_FREE; return 0; }
static unsigned fsize(FILE *f) { return f->kind == K_RAM ? f->rf->size : f->size; }
size_t fread(void *p, size_t sz, size_t n, FILE *f) {
  unsigned char *d = p; size_t want = sz * n, got = 0;
  if (!want || f->kind == K_OUT || f->kind == K_IN) return 0;
  if (f->pos >= fsize(f)) { f->eof = 1; return 0; }
  if (want > fsize(f) - f->pos) want = fsize(f) - f->pos;
  if (f->kind == K_RAM) { memcpy(d, f->rf->data + f->pos, want); f->pos += (unsigned)want; return want / sz; }
  while (got < want) {
    unsigned off = f->pos % SECT, lba = f->lba + f->pos / SECT, left = (unsigned)(want - got), k;
    if (off == 0 && left >= SECT) { k = left / SECT; if (dvd_read(lba, k, d + got)) { f->err = 1; break; } k *= SECT; }
    else {
      if (!f->sect) f->sect = malloc(SECT);
      if (f->sect_lba != lba) { if (dvd_read(lba, 1, f->sect)) { f->err = 1; break; } f->sect_lba = lba; }
      k = SECT - off; if (k > left) k = left; memcpy(d + got, f->sect + off, k);
    }
    got += k; f->pos += k;
  }
  return got / sz;
}
size_t fwrite(const void *p, size_t sz, size_t n, FILE *f) {
  size_t t = sz * n; const unsigned char *s = p;
  if (f->kind == K_OUT) { size_t i; for (i = 0; i < t; i++) putchar(s[i]); return n; }
  if (f->kind != K_RAM || !f->writable) { f->err = 1; return 0; }
  if (f->pos + t > f->rf->cap) { unsigned nc = (f->pos + (unsigned)t) * 2 + 256; unsigned char *nd = realloc(f->rf->data, nc); if (!nd) { f->err = 1; return 0; } f->rf->data = nd; f->rf->cap = nc; }
  if (f->pos > f->rf->size) memset(f->rf->data + f->rf->size, 0, f->pos - f->rf->size);
  memcpy(f->rf->data + f->pos, s, t); f->pos += (unsigned)t; if (f->pos > f->rf->size) f->rf->size = f->pos;
  return n;
}
int fseek(FILE *f, long off, int wh) {
  long np = wh == SEEK_SET ? off : wh == SEEK_CUR ? (long)f->pos + off : (long)fsize(f) + off;
  if (np < 0) { errno = EINVAL; return -1; }
  f->pos = (unsigned)np; f->eof = 0; f->unget = -1; return 0;
}
long ftell(FILE *f) { return (long)f->pos - (f->unget >= 0); }
void rewind(FILE *f) { fseek(f, 0, SEEK_SET); f->err = 0; }
int fflush(FILE *f) { (void)f; return 0; }
int feof(FILE *f) { return f->eof; }
int ferror(FILE *f) { return f->err; }
void clearerr(FILE *f) { f->eof = f->err = 0; }
int fileno(FILE *f) { return (int)(f - files); }
int setvbuf(FILE *f, char *b, int m, size_t s) { (void)f; (void)b; (void)m; (void)s; return 0; }
int fgetc(FILE *f) { unsigned char c; if (f->unget >= 0) { c = (unsigned char)f->unget; f->unget = -1; return c; } return fread(&c, 1, 1, f) == 1 ? c : EOF; }
int getc(FILE *f) { return fgetc(f); }
int ungetc(int c, FILE *f) { if (c == EOF) return EOF; f->unget = c; f->eof = 0; return c; }
int fputc(int c, FILE *f) { unsigned char ch = (unsigned char)c; return fwrite(&ch, 1, 1, f) == 1 ? c : EOF; }
int putc(int c, FILE *f) { return fputc(c, f); }
int fputs(const char *s, FILE *f) { return fwrite(s, 1, strlen(s), f) ? 0 : EOF; }
char *fgets(char *s, int n, FILE *f) { int i = 0, c; while (i < n - 1 && (c = fgetc(f)) != EOF) { s[i++] = (char)c; if (c == '\n') break; } if (!i) return 0; s[i] = 0; return s; }
int vfprintf(FILE *f, const char *fmt, va_list ap) {
  if (f->kind == K_OUT) return vprintf(fmt, ap);
  { va_list ap2; int n; char *b; va_copy(ap2, ap); n = vsnprintf(0, 0, fmt, ap2); va_end(ap2); b = malloc((size_t)n + 1); if (!b) return -1; vsnprintf(b, (size_t)n + 1, fmt, ap); fwrite(b, 1, (size_t)n, f); free(b); return n; }
}
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; int r; va_start(ap, fmt); r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int remove(const char *p) { struct ramfile *r = ram_find(p); if (!r) { errno = ENOENT; return -1; } free(r->data); memset(r, 0, sizeof *r); return 0; }
int rename(const char *a, const char *b) { struct ramfile *r = ram_find(a); if (!r) { errno = ENOENT; return -1; } strncpy(r->name, b, 127); return 0; }
void perror(const char *s) { printf("%s: %s\n", s, strerror(errno)); }
int stat(const char *p, struct stat *st) {
  struct ramfile *r = ram_find(p); int i;
  if (r) { st->st_size = (long)r->size; st->st_mode = S_IFREG | 0644; st->st_mtime = EPOCH0; return 0; }
  i = sf_fs_find(p); if (i >= 0) { st->st_size = (long)dir[i].size; st->st_mode = S_IFREG | 0444; st->st_mtime = EPOCH0; return 0; }
  errno = ENOENT; return -1;
}
int mkdir(const char *p, mode_t m) { (void)p; (void)m; return 0; }
mode_t umask(mode_t m) { return m; }
int access(const char *p, int m) { struct stat st; (void)m; return stat(p, &st); }
char *getcwd(char *b, size_t n) { if (n < 2) return 0; b[0] = '/'; b[1] = 0; return b; }
int chdir(const char *p) { (void)p; return 0; }
unsigned sleep(unsigned s) { unsigned long long end = sf_time_ns() + (unsigned long long)s * 1000000000ull; while (sf_time_ns() < end) { } return 0; }
int usleep(unsigned us) { unsigned long long end = sf_time_ns() + (unsigned long long)us * 1000ull; while (sf_time_ns() < end) { } return 0; }

/* ------------------------------ math extra ------------------------------ */
double hypot(double x, double y) { return sqrt(x * x + y * y); }
double cbrt(double x) { double r; int neg = x < 0, i; if (neg) x = -x; if (x == 0) return 0; r = x > 1 ? x / 3 : 1; for (i = 0; i < 60; i++) r = (2 * r + x / (r * r)) / 3; return neg ? -r : r; }
double log2(double x) { return log(x) / 0.693147180559945309417; }
double exp2(double x) { return exp(x * 0.693147180559945309417); }
double copysign(double x, double y) { return (y < 0 || (y == 0 && 1 / y < 0)) ? -fabs(x) : fabs(x); }
double fmax(double a, double b) { return a > b ? a : b; }
double fmin(double a, double b) { return a < b ? a : b; }
double modf(double x, double *ip) { double t = trunc(x); *ip = t; return x - t; }
double asinh(double x) { return log(x + sqrt(x * x + 1)); }
double acosh(double x) { return log(x + sqrt(x * x - 1)); }
double atanh(double x) { return 0.5 * log((1 + x) / (1 - x)); }
float fmaxf(float a, float b) { return a > b ? a : b; } float fminf(float a, float b) { return a < b ? a : b; } float roundf(float x) { return (float)round(x); } float truncf(float x) { return (float)trunc(x); }
float hypotf(float x, float y) { return (float)hypot(x, y); } float ldexpf(float x, int e) { return (float)ldexp(x, e); } float frexpf(float x, int *e) { return (float)frexp(x, e); }
float modff(float x, float *ip) { double i; double r = modf(x, &i); *ip = (float)i; return (float)r; }
long lrint(double x) { double r = floor(x + 0.5); if (r - x == 0.5 && ((long)r & 1)) r -= 1; return (long)r; }
long lrintf(float x) { return lrint(x); }

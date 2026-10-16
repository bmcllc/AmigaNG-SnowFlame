/* libc_test.c - exercita a libc ampliada do runtime SF-100 (a saída é a mesma do glibc no host, exceto a parte do DVD, só em __m68k__). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>
#include <time.h>
#include <math.h>
#include <errno.h>

static jmp_buf env;
static int depth(int n) { if (n == 0) longjmp(env, 7); return depth(n - 1) + 1; }
static int cmpi(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(void) {
  int a, b, n; unsigned u; long long ll; float f; double d; char s[32], t[32], c;
  n = sscanf("12 -34 0x1F 077 4.5e1 word [abc]", "%d %d %i %i %lf %s [%[^]]", &a, &b, &(int){0}, &(int){0}, &d, s, t);
  printf("sscanf: n=%d a=%d b=%d d=%.1f s=%s t=%s\n", n, a, b, d, s, t);
  n = sscanf("x=0042;y=-7 z", "x=%u;y=%d %c", &u, &a, &c); printf("sscanf2: n=%d u=%u a=%d c=%c\n", n, u, a, c);
  n = sscanf("123456789012 3.25", "%lld %f", &ll, &f); printf("sscanf3: n=%d ll=%lld f=%.2f\n", n, ll, f);
  printf("strtod: %.6f %.6f %.3e %.1f\n", strtod("3.14159abc", 0), strtod("-.5e1", 0), strtod("1e-5", 0), strtod("  +100", 0));
  printf("strtol: %ld %ld %ld %lu\n", strtol("-0x1f", 0, 0), strtol("777", 0, 8), strtol("zz", 0, 36), strtoul("4000000000", 0, 10));
  printf("ctype: %d%d%d%d%d %c%c\n", isalpha('a') != 0, isdigit('x') != 0, isspace('\t') != 0, ispunct('!') != 0, isxdigit('F') != 0, toupper('q'), tolower('Q'));
  { volatile int r = setjmp(env); if (r == 0) depth(5); printf("longjmp valor=%d\n", r); }
  { time_t t = 1700000000; struct tm *tm = gmtime(&t); char buf[64]; strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %a %b", tm); printf("gmtime: %s yday=%d wday=%d mktime=%ld\n", buf, tm->tm_yday, tm->tm_wday, (long)mktime(tm)); }
  { int v[9] = {9, 3, 7, 1, 8, 2, 6, 4, 5}, k = 6, *p; qsort(v, 9, sizeof v[0], cmpi); p = bsearch(&k, v, 9, sizeof v[0], cmpi); printf("qsort/bsearch: %d%d%d %d\n", v[0], v[4], v[8], p ? (int)(p - v) : -1); }
  { FILE *fp = fopen("/tmp/x.cfg", "w"); char line[64]; long pos; fprintf(fp, "seta name \"%s\"\nseta skill %d\n", "Visor", 3); fputs("fim", fp); fclose(fp);
    fp = fopen("/tmp/x.cfg", "r"); fgets(line, sizeof line, fp); printf("ram file: %s", line); pos = ftell(fp); fgets(line, sizeof line, fp); printf("ram file: %s", line);
    fseek(fp, -3, SEEK_END); { int c1 = fgetc(fp), c2 = fgetc(fp), c3 = fgetc(fp); long p2 = ftell(fp); printf("tail=%c%c%c pos=%ld/%ld eof=%d\n", c1, c2, c3, p2, pos, feof(fp)); } fgetc(fp); printf("eof=%d\n", feof(fp)); fclose(fp);
    fp = fopen("/nao/existe", "r"); printf("fopen falha: %d errno=%d\n", fp == 0, errno); }
  printf("snprintf: [%8.3f] [%-8d] [%+d] [%08.2f] [%.0f] [%5s] [%.2s] [%e]\n", 3.14159, 42, 5, -2.5, 2.5, "ab", "xyz", 123456.789);
  printf("math: %.6f %.6f %.6f %.6f %.6f %.6f\n", hypot(3, 4), cbrt(27.0), log2(1024.0), atan(1.0) * 4, fmod(10.5, 3), tanh(0.5));
#ifdef __m68k__
  { FILE *fp = fopen("sys/hello.txt", "rb"); char buf[80]; size_t nr; if (fp) { nr = fread(buf, 1, sizeof buf - 1, fp); buf[nr] = 0; printf("dvd: %zu bytes, size via fseek=%ld: %s", nr, (fseek(fp, 0, SEEK_END), ftell(fp)), buf); fclose(fp); } else printf("dvd: sem arquivo\n"); }
  { FILE *fp = fopen("data/big.bin", "rb"); unsigned char b2[5000]; unsigned sum = 0; int i; size_t nr; if (fp) { fseek(fp, 7, SEEK_SET); nr = fread(b2, 1, sizeof b2, fp); for (i = 0; i < (int)nr; i++) sum += b2[i] * (i + 1); printf("dvd big: %zu bytes sum=%u\n", nr, sum); fclose(fp); } }
#endif
  return 0;
}

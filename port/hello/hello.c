/* hello.c - primeiro programa C (GCC, ColdFire V4e real) no SF-100: libc/libm do runtime, FPU, inteiros de 64 bits, malloc e qsort. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
struct vec { float x, y, z; };
static float dot(struct vec a, struct vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }   /* struct por valor: o snowcc não aceita, o GCC sim */

int main(void) {
  int i, v[16]; char buf[64]; long long big = 1000000007LL * 998244353LL; unsigned long long ub = 0xFFFFFFFFFFFFFFFFULL;
  struct vec a = {1.5f, -2.0f, 0.25f}, b = {2.0f, 0.5f, 4.0f};
  printf("Ola, SnowFlame SF-100! (GCC, ColdFire V4e)\n");
  printf("sqrt(2)=%.12f  sin(1)=%.12f  cos(1)=%.12f  atan2(1,2)=%.12f\n", sqrt(2.0), sin(1.0), cos(1.0), atan2(1.0, 2.0));
  printf("exp(1.5)=%.10f  log(10)=%.10f  pow(2,0.5)=%.10f  pow(3,5)=%.1f  floor(-2.5)=%.1f\n", exp(1.5), log(10.0), pow(2.0, 0.5), pow(3.0, 5.0), floor(-2.5));
  printf("big=%lld  big/12345=%lld  big%%1000003=%lld  ub/3=%llu  ub>>33=%llu\n", big, big / 12345, big % 1000003, ub / 3, ub >> 33);
  printf("dot=%.3f  float->int %d  %g %g %e\n", (double)dot(a, b), (int)(3.99f), 0.0001234, 123456789.0, 12345.678);
  for (i = 0; i < 16; i++) v[i] = (i * 7919 + 13) % 101;
  qsort(v, 16, sizeof v[0], cmp_int);
  buf[0] = 0; for (i = 0; i < 16; i++) { char t[8]; sprintf(t, "%d ", v[i]); strcat(buf, t); }
  printf("qsort: %s\n", buf);
  { char *p = malloc(1000), *q = malloc(5000); memset(p, 'x', 999); p[999] = 0; free(p); q = realloc(q, 9000); printf("malloc ok: %d %d\n", (int)strlen(q ? "ok" : ""), (int)(q != 0)); free(q); }
  printf("%5d|%-5d|%05d|%x|%#X|%c|%10.3f|%s\n", 42, 42, 42, 255, 255, 'Z', 3.14159265, "fim");
  return 42;
}

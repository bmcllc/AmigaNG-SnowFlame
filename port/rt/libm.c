/* libm.c - libm mínima em double para o runtime SF-100. O ColdFire V4e só tem FSQRT/FABS/FINT na FPU (sem transcendentais): o resto é série/polinômio. */
#include <math.h>
#include <stdint.h>

#define PI 3.14159265358979323846
#define HALFPI 1.57079632679489661923
#define LN2 0.693147180559945309417

union dbl { double d; uint64_t u; struct { uint32_t hi, lo; } w; };      /* big-endian: palavra alta primeiro */
int isnan(double x) { return x != x; }
int isinf(double x) { return x > 1.7976931348623157e308 || x < -1.7976931348623157e308; }
double fabs(double x) { return __builtin_fabs(x); }
double sqrt(double x) { return __builtin_sqrt(x); }
double floor(double x) {
  union dbl v; int e;
  v.d = x; e = (int)((v.w.hi >> 20) & 0x7FF) - 1023;
  if (e >= 52) return x;
  if (e < 0) return x < 0 ? -1.0 : (x == 0 ? x : 0.0);
  { double t = (x > -2147483648.0 && x < 2147483647.0) ? (double)(int)x : (double)(long long)x; return t > x ? t - 1.0 : t; }
}
double ceil(double x) { return -floor(-x); }
double trunc(double x) { return x < 0 ? ceil(x) : floor(x); }
double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }
double fmod(double x, double y) { if (y == 0) return __builtin_nan(""); return x - trunc(x / y) * y; }
double ldexp(double x, int e) {
  union dbl v; int ex;
  if (x == 0 || e == 0) return x;
  v.d = x; ex = (int)((v.w.hi >> 20) & 0x7FF);
  if (ex == 0) { x *= 18014398509481984.0; e -= 54; v.d = x; ex = (int)((v.w.hi >> 20) & 0x7FF); }
  ex += e;
  if (ex >= 0x7FF) return x < 0 ? -HUGE_VAL : HUGE_VAL;
  if (ex <= 0) { if (ex <= -54) return 0; v.w.hi = (v.w.hi & 0x800FFFFFu) | (1u << 20); v.d = v.d; { double r; union dbl t; t.d = x; t.w.hi = (t.w.hi & 0x800FFFFFu) | (uint32_t)((ex + 54) << 20); r = t.d; return r * (1.0 / 18014398509481984.0); } }
  v.w.hi = (v.w.hi & 0x800FFFFFu) | ((uint32_t)ex << 20); return v.d;
}
double frexp(double x, int *e) {
  union dbl v; int ex;
  if (x == 0) { *e = 0; return 0; }
  v.d = x; ex = (int)((v.w.hi >> 20) & 0x7FF);
  if (ex == 0) { x *= 18014398509481984.0; v.d = x; ex = (int)((v.w.hi >> 20) & 0x7FF) - 54; }
  *e = ex - 1022; v.w.hi = (v.w.hi & 0x800FFFFFu) | (1022u << 20); return v.d;
}

/* seno/cosseno: redução a [-pi/4, pi/4] e série de Taylor de grau alto (erro ~1e-15) */
static double sin_k(double x) { double x2 = x * x; return x * (1 + x2 * (-1.0 / 6 + x2 * (1.0 / 120 + x2 * (-1.0 / 5040 + x2 * (1.0 / 362880 + x2 * (-1.0 / 39916800 + x2 * (1.0 / 6227020800.0 + x2 * (-1.0 / 1307674368000.0)))))))); }
static double cos_k(double x) { double x2 = x * x; return 1 + x2 * (-0.5 + x2 * (1.0 / 24 + x2 * (-1.0 / 720 + x2 * (1.0 / 40320 + x2 * (-1.0 / 3628800 + x2 * (1.0 / 479001600 + x2 * (-1.0 / 87178291200.0))))))); }
static double trig(double x, int want_cos) {
  double q, r, nd; int quad;
  if (x != x || isinf(x)) return __builtin_nan("");
  q = x / HALFPI;
  if (q > -1e9 && q < 1e9) { int ni = (int)(q < 0 ? q - 0.5 : q + 0.5); nd = (double)ni; quad = ni & 3; }    /* caminho comum: conversões nativas da FPU */
  else { long long n = (long long)(q < 0 ? q - 0.5 : q + 0.5); nd = (double)n; quad = (int)(n & 3); }
  r = x - nd * 1.57079632679489655800 - nd * 6.12323399573676603587e-17;   /* pi/2 em duas partes */
  if (want_cos) quad = (quad + 1) & 3;
  switch (quad) { case 0: return sin_k(r); case 1: return cos_k(r); case 2: return -sin_k(r); default: return -cos_k(r); }
}
double sin(double x) { return trig(x, 0); }
double cos(double x) { return trig(x, 1); }
double tan(double x) { return sin(x) / cos(x); }

double atan(double x) {
  int neg = 0, inv = 0, k = 0; double r, x2;
  if (x != x) return x;
  if (x < 0) { neg = 1; x = -x; }
  if (x > 1e300) r = HALFPI;
  else {
    if (x > 1) { x = 1 / x; inv = 1; }
    while (x > 0.2) { x = x / (1 + sqrt(1 + x * x)); k++; }               /* atan(x) = 2*atan(x/(1+sqrt(1+x^2))) */
    x2 = x * x; r = x * (1 + x2 * (-1.0 / 3 + x2 * (1.0 / 5 + x2 * (-1.0 / 7 + x2 * (1.0 / 9 + x2 * (-1.0 / 11 + x2 * (1.0 / 13)))))));
    while (k--) r *= 2;
    if (inv) r = HALFPI - r;
  }
  return neg ? -r : r;
}
double atan2(double y, double x) {
  if (x > 0) return atan(y / x);
  if (x < 0) return y >= 0 ? atan(y / x) + PI : atan(y / x) - PI;
  if (y > 0) return HALFPI; if (y < 0) return -HALFPI; return 0;
}
double asin(double x) { if (x >= 1) return HALFPI; if (x <= -1) return -HALFPI; return atan(x / sqrt(1 - x * x)); }
double acos(double x) { if (x >= 1) return 0; if (x <= -1) return PI; return HALFPI - asin(x); }

double exp(double x) {
  double r, t; int k, i;
  if (x != x) return x;
  if (x > 709.78) return HUGE_VAL; if (x < -745) return 0;
  k = (int)(x / LN2 + (x < 0 ? -0.5 : 0.5)); r = x - k * LN2;
  t = 1; for (i = 17; i >= 1; i--) t = 1 + r * t / i;
  return ldexp(t, k);
}
double log(double x) {
  int e; double m, f, s, s2, r;
  if (x != x || x < 0) return __builtin_nan("");
  if (x == 0) return -HUGE_VAL;
  m = frexp(x, &e); if (m < 0.70710678118654752) { m *= 2; e--; }
  f = (m - 1) / (m + 1); s = f; s2 = f * f; r = 0;
  { int i; double p = f; for (i = 1; i <= 41; i += 2) { r += p / i; p *= s2; } }
  (void)s;
  return 2 * r + e * LN2;
}
double log10(double x) { return log(x) / 2.30258509299404568402; }
double pow(double x, double y) {
  if (y == 0) return 1; if (x == 1) return 1;
  if (y == (double)(long long)y && y > -1024 && y < 1024) {                  /* expoente inteiro: multiplicação binária */
    long long n = (long long)y; int neg = n < 0; double r = 1, b = x; if (neg) n = -n;
    while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
    return neg ? 1 / r : r;
  }
  if (x == 0) return y > 0 ? 0 : HUGE_VAL;
  if (x < 0) return __builtin_nan("");
  return exp(y * log(x));
}
double sinh(double x) { double e = exp(x); return (e - 1 / e) / 2; }
double cosh(double x) { double e = exp(x); return (e + 1 / e) / 2; }
double tanh(double x) { if (x > 20) return 1; if (x < -20) return -1; { double e = exp(2 * x); return (e - 1) / (e + 1); } }

float sqrtf(float x) { return (float)sqrt(x); } float sinf(float x) { return (float)sin(x); } float cosf(float x) { return (float)cos(x); }
float tanf(float x) { return (float)tan(x); } float atanf(float x) { return (float)atan(x); } float atan2f(float y, float x) { return (float)atan2(y, x); }
float asinf(float x) { return (float)asin(x); } float acosf(float x) { return (float)acos(x); } float expf(float x) { return (float)exp(x); }
float logf(float x) { return (float)log(x); } float powf(float x, float y) { return (float)pow(x, y); } float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); } float fabsf(float x) { return __builtin_fabsf(x); } float fmodf(float x, float y) { return (float)fmod(x, y); }

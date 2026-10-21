/* libgcc.c - substitutos das rotinas de libgcc que o GCC chama no ColdFire (o libgcc da distribuição é de 68020, não serve): inteiros de 64 bits e conversões. */
#include <stdint.h>

typedef unsigned long long u64; typedef long long i64; typedef unsigned u32; typedef int i32;

u64 __muldi3(u64 a, u64 b) {                                       /* 64x64 -> 64 com multiplicações de 32 bits */
  u32 al = (u32)a, ah = (u32)(a >> 32), bl = (u32)b, bh = (u32)(b >> 32);
  u32 a0 = al & 0xFFFF, a1 = al >> 16, b0 = bl & 0xFFFF, b1 = bl >> 16;
  u32 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  u32 mid = (p00 >> 16) + (p01 & 0xFFFF) + (p10 & 0xFFFF);
  u32 lo = (p00 & 0xFFFF) | (mid << 16);
  u32 hi = p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16);
  hi += al * bh + ah * bl;
  return ((u64)hi << 32) | lo;
}
u64 __udivmoddi4(u64 n, u64 d, u64 *rem) {
  u64 q = 0, r = 0; int i;
  if (d == 0) { if (rem) *rem = 0; return 0; }
  if ((n >> 32) == 0 && (d >> 32) == 0) { u32 nn = (u32)n, dd = (u32)d; if (rem) *rem = nn % dd; return nn / dd; }
  if ((d >> 32) == 0) {                                            /* divisor de 32 bits: parte alta por divisão nativa, parte baixa em 32 passos de 32 bits */
    u32 dd = (u32)d, nl = (u32)n, qh = (u32)(n >> 32) / dd, r32 = (u32)(n >> 32) % dd, ql = 0;
    for (i = 31; i >= 0; i--) { u32 carry = r32 >> 31; r32 = (r32 << 1) | (nl >> 31); nl <<= 1; if (carry || r32 >= dd) { r32 -= dd; ql |= 1u << i; } }
    if (rem) *rem = r32;
    return ((u64)qh << 32) | ql;
  }
  for (i = 63; i >= 0; i--) { r = (r << 1) | ((n >> i) & 1); if (r >= d) { r -= d; q |= (u64)1 << i; } }
  if (rem) *rem = r;
  return q;
}
u64 __udivdi3(u64 n, u64 d) { return __udivmoddi4(n, d, 0); }
u64 __umoddi3(u64 n, u64 d) { u64 r; __udivmoddi4(n, d, &r); return r; }
i64 __divdi3(i64 n, i64 d) { int neg = (n < 0) ^ (d < 0); u64 q = __udivmoddi4(n < 0 ? -(u64)n : (u64)n, d < 0 ? -(u64)d : (u64)d, 0); return neg ? -(i64)q : (i64)q; }
i64 __moddi3(i64 n, i64 d) { u64 r; __udivmoddi4(n < 0 ? -(u64)n : (u64)n, d < 0 ? -(u64)d : (u64)d, &r); return n < 0 ? -(i64)r : (i64)r; }
i64 __ashldi3(i64 v, int s) { u32 lo = (u32)v, hi = (u32)((u64)v >> 32); if (s >= 32) { hi = lo << (s - 32); lo = 0; } else if (s) { hi = (hi << s) | (lo >> (32 - s)); lo <<= s; } return (i64)(((u64)hi << 32) | lo); }
u64 __lshrdi3(u64 v, int s) { u32 lo = (u32)v, hi = (u32)(v >> 32); if (s >= 32) { lo = hi >> (s - 32); hi = 0; } else if (s) { lo = (lo >> s) | (hi << (32 - s)); hi >>= s; } return ((u64)hi << 32) | lo; }
i64 __ashrdi3(i64 v, int s) { i32 hi = (i32)((u64)v >> 32); u32 lo = (u32)v; if (s >= 32) { lo = (u32)(hi >> (s - 32 > 31 ? 31 : s - 32)); hi = hi < 0 ? -1 : 0; } else if (s) { lo = (lo >> s) | ((u32)hi << (32 - s)); hi >>= s; } return (i64)(((u64)(u32)hi << 32) | lo); }
int __cmpdi2(i64 a, i64 b) { return a < b ? 0 : a == b ? 1 : 2; }
int __ucmpdi2(u64 a, u64 b) { return a < b ? 0 : a == b ? 1 : 2; }

double __floatundidf(u64 v) { if (!(v >> 32)) return (double)(u32)v; return (double)(u32)(v >> 32) * 4294967296.0 + (double)(u32)v; }
double __floatdidf(i64 v) { if (v >= -2147483648LL && v <= 2147483647LL) return (double)(i32)v; return v < 0 ? -__floatundidf(-(u64)v) : __floatundidf((u64)v); }
float __floatundisf(u64 v) { return (float)__floatundidf(v); }
float __floatdisf(i64 v) { return (float)__floatdidf(v); }
u64 __fixunsdfdi(double d) {
  if (d <= 0) return 0;
  if (d < 2147483648.0) return (u32)(i32)d;
  if (d >= 18446744073709551616.0) return ~(u64)0;
  { double hi = d / 4294967296.0; u32 h = (u32)hi; double lo = d - (double)h * 4294967296.0; return ((u64)h << 32) | (u32)lo; }
}
i64 __fixdfdi(double d) { if (d > -2147483648.0 && d < 2147483648.0) return (i32)d; return d < 0 ? -(i64)__fixunsdfdi(-d) : (i64)__fixunsdfdi(d); }
i64 __fixsfdi(float f) { return __fixdfdi((double)f); }
u64 __fixunssfdi(float f) { return __fixunsdfdi((double)f); }

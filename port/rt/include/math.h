#ifndef SF_MATH_H
#define SF_MATH_H
#define M_PI 3.14159265358979323846
#define M_E 2.71828182845904523536
#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
double sqrt(double), sin(double), cos(double), tan(double), atan(double), atan2(double, double), asin(double), acos(double);
double exp(double), log(double), log10(double), pow(double, double), floor(double), ceil(double), fabs(double), fmod(double, double), ldexp(double, int), frexp(double, int *);
double round(double), trunc(double), sinh(double), cosh(double), tanh(double);
float sqrtf(float), sinf(float), cosf(float), tanf(float), atanf(float), atan2f(float, float), asinf(float), acosf(float), expf(float), logf(float), powf(float, float), floorf(float), ceilf(float), fabsf(float), fmodf(float, float);
double hypot(double, double), cbrt(double), log2(double), exp2(double), copysign(double, double), fmax(double, double), fmin(double, double), modf(double, double *), asinh(double), acosh(double), atanh(double);
float fmaxf(float, float), fminf(float, float), roundf(float), truncf(float), hypotf(float, float), ldexpf(float, int), frexpf(float, int *), modff(float, float *);
long lrint(double);
long lrintf(float);
#define isfinite(x) (!isnan(x) && !isinf(x))
int isnan(double);
int isinf(double);
#endif

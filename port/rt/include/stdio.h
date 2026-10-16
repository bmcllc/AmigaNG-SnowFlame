#ifndef SF_STDIO_H
#define SF_STDIO_H
#include <stddef.h>
#include <stdarg.h>
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int sprintf(char *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *b, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *b, size_t n, const char *fmt, va_list ap);
int vsprintf(char *b, const char *fmt, va_list ap);
int vprintf(const char *fmt, va_list ap);
int putchar(int c);
int puts(const char *s);
#endif

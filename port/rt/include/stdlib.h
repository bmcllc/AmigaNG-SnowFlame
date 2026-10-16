#ifndef SF_STDLIB_H
#define SF_STDLIB_H
#include <stddef.h>
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *realloc(void *p, size_t n);
void free(void *p);
void abort(void) __attribute__((noreturn));
void exit(int code) __attribute__((noreturn));
int abs(int v);
long labs(long v);
int atoi(const char *s);
long strtol(const char *s, char **end, int base);
double atof(const char *s);
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
int rand(void);
void srand(unsigned s);
double strtod(const char *s, char **end);
float strtof(const char *s, char **end);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
long atol(const char *s);
char *getenv(const char *name);
int system(const char *cmd);
void *bsearch(const void *key, const void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
int atexit(void (*fn)(void));
#define RAND_MAX 0x7FFFFFFF
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#endif

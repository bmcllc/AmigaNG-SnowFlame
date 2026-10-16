#ifndef SF_STDIO_H
#define SF_STDIO_H
#include <stddef.h>
#include <stdarg.h>
typedef struct sf_FILE FILE;
extern FILE *stdin, *stdout, *stderr;
#define EOF (-1)
#define BUFSIZ 1024
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define FILENAME_MAX 256
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define FOPEN_MAX 32
typedef long fpos_t;
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int sprintf(char *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *b, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vsnprintf(char *b, size_t n, const char *fmt, va_list ap);
int vsprintf(char *b, const char *fmt, va_list ap);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int sscanf(const char *s, const char *fmt, ...);
int vsscanf(const char *s, const char *fmt, va_list ap);
int putchar(int c);
int puts(const char *s);
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *p, size_t sz, size_t n, FILE *f);
size_t fwrite(const void *p, size_t sz, size_t n, FILE *f);
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int fflush(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
int ungetc(int c, FILE *f);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int fputs(const char *s, FILE *f);
char *fgets(char *s, int n, FILE *f);
int remove(const char *path);
int rename(const char *a, const char *b);
void perror(const char *s);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
int fileno(FILE *f);
#endif

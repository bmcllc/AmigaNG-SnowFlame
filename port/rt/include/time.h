#ifndef SF_TIME_H
#define SF_TIME_H
#include <stddef.h>
typedef long time_t;
typedef long clock_t;
#define CLOCKS_PER_SEC 1000000L
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
time_t time(time_t *t);
clock_t clock(void);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
time_t mktime(struct tm *tm);
char *ctime(const time_t *t);
char *asctime(const struct tm *tm);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
double difftime(time_t a, time_t b);
#endif

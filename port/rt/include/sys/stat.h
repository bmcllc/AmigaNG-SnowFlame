#ifndef SF_SYS_STAT_H
#define SF_SYS_STAT_H
#include <sys/types.h>
struct stat { long st_size; unsigned st_mode; long st_mtime; };
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
int stat(const char *path, struct stat *st);
int mkdir(const char *path, mode_t mode);
mode_t umask(mode_t m);
#endif

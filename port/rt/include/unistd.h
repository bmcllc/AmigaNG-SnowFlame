#ifndef SF_UNISTD_H
#define SF_UNISTD_H
#include <sys/types.h>
int access(const char *path, int mode);
char *getcwd(char *buf, size_t n);
int chdir(const char *p);
unsigned sleep(unsigned s);
int usleep(unsigned us);
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
#endif

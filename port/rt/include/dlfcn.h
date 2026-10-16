#ifndef SF_DLFCN_H
#define SF_DLFCN_H
#define RTLD_NOW 2
void *dlopen(const char *f, int m);
int dlclose(void *h);
void *dlsym(void *h, const char *s);
char *dlerror(void);
#endif

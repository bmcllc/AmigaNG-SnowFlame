#ifndef SF_SIGNAL_H
#define SF_SIGNAL_H
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIGINT 2
#define SIGILL 4
#define SIGFPE 8
#define SIGSEGV 11
#define SIGTERM 15
sighandler_t signal(int sig, sighandler_t h);
#endif

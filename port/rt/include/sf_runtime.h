/* sf_runtime.h - interface do runtime freestanding do SF-100 para programas compilados com o GCC (ColdFire V4e). */
#ifndef SF_RUNTIME_H
#define SF_RUNTIME_H
#define SF_DBG_PUTC (*(volatile unsigned *)0xF00E0000u)         /* console de depuração */
extern char _end[];
extern char __code_end[];
void __rt_init(void);
#endif

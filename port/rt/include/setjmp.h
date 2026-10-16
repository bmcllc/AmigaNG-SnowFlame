#ifndef SF_SETJMP_H
#define SF_SETJMP_H
typedef unsigned long jmp_buf[14];     /* d2-d7, a2-a7 (sp salvo aponta para o endereço de retorno) */
int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
#endif

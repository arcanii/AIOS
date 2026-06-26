/* setjmp.h -- AIOS shadow header. jmp_buf holds the aarch64 callee-saved regs + sp + lr + d8-d15
 * (>= 22 doublewords; see the asm in libaios.c). sigsetjmp/siglongjmp ignore the savemask (no
 * signal-mask model yet) and alias setjmp/longjmp. dash's exception mechanism (error.c) needs these. */
#ifndef _SETJMP_H
#define _SETJMP_H

typedef unsigned long jmp_buf[32];
typedef unsigned long sigjmp_buf[32];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
int  sigsetjmp(sigjmp_buf env, int savemask);
void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));

#endif /* _SETJMP_H */

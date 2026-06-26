/* alloca.h -- AIOS shadow header. alloca must allocate on the CALLER's stack, so it is the compiler
 * builtin (a libc function could not). dash's parser.c includes this. */
#ifndef _ALLOCA_H
#define _ALLOCA_H
#include <stddef.h>

#ifndef alloca
#define alloca(size) __builtin_alloca(size)
#endif

#endif /* _ALLOCA_H */

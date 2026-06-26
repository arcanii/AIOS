/* regex.h -- AIOS shadow header (see sys/types.h). Backed by a real BRE/ERE engine in libaios
 * (regcomp/regexec/regfree/regerror -- a linear NFA-simulation matcher; see libaios.c) so vendored
 * `grep` runs UNMODIFIED. Boolean match only today: grep compiles REG_NOSUB and never reads pmatch,
 * so regexec sets pmatch[*] = {-1,-1} (submatch capture is not yet implemented). */
#ifndef _REGEX_H
#define _REGEX_H
#include <stddef.h>

typedef long regoff_t;
typedef struct {
    size_t re_nsub;
    void  *__impl;
} regex_t;
typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

/* regcomp cflags */
#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NOSUB    4
#define REG_NEWLINE  8
/* regexec eflags */
#define REG_NOTBOL   1
#define REG_NOTEOL   2
/* error codes (glibc-compatible values; regcomp returns one, regerror maps it to a message) */
#define REG_NOMATCH   1
#define REG_BADPAT    2
#define REG_ECOLLATE  3
#define REG_ECTYPE    4
#define REG_EESCAPE   5
#define REG_ESUBREG   6
#define REG_EBRACK    7
#define REG_EPAREN    8
#define REG_EBRACE    9
#define REG_BADBR    10
#define REG_ERANGE   11
#define REG_ESPACE   12
#define REG_BADRPT   13

int    regcomp(regex_t *, const char *, int);
int    regexec(const regex_t *, const char *, size_t, regmatch_t *, int);
size_t regerror(int, const regex_t *, char *, size_t);
void   regfree(regex_t *);

#endif /* _REGEX_H */

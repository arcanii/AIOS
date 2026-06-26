/* regex.h -- AIOS shadow header (see sys/types.h). DECLARATIONS ONLY: sbase's util.h includes
 * <regex.h> for the regex_t type so it can declare eregcomp/enregcomp, but utilities that do not
 * actually match (echo, cat, wc, ...) never call regcomp/regexec, so no implementation is linked.
 * The day a regex-using utility (grep) is vendored, libaios grows a real regex -- the sources stay
 * unmodified. */
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
/* error codes */
#define REG_NOMATCH  1

int    regcomp(regex_t *, const char *, int);
int    regexec(const regex_t *, const char *, size_t, regmatch_t *, int);
size_t regerror(int, const regex_t *, char *, size_t);
void   regfree(regex_t *);

#endif /* _REGEX_H */

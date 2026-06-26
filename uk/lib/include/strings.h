/* strings.h -- AIOS shadow header (see sys/types.h). The BSD/POSIX case-insensitive string ops,
 * implemented in libaios. grep includes <strings.h> for these (its -i / -F paths). strcasecmp and
 * strncasecmp are also declared by <string.h>; identical prototypes, so including both is fine. */
#ifndef _STRINGS_H
#define _STRINGS_H
#include <stddef.h>

int   strcasecmp(const char *a, const char *b);
int   strncasecmp(const char *a, const char *b, size_t n);
char *strcasestr(const char *haystack, const char *needle);

#endif /* _STRINGS_H */

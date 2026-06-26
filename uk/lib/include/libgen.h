/* libgen.h -- AIOS shadow header (see sys/types.h). Path component splitting; sbase's
 * libutil/enmasse.c uses basename(). Both may modify the passed buffer (POSIX). */
#ifndef _LIBGEN_H
#define _LIBGEN_H

char *basename(char *path);
char *dirname(char *path);

#endif /* _LIBGEN_H */

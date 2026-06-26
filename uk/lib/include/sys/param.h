/* sys/param.h -- AIOS shadow header. dash's shell.h pulls this in for MAXPATHLEN + MIN/MAX. */
#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H
#include <limits.h>

#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif
#ifndef NOFILE
#define NOFILE 64
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#endif /* _SYS_PARAM_H */

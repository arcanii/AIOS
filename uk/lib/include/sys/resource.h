/* sys/resource.h -- AIOS shadow header. Minimal rlimit so dash's ulimit builtin compiles + runs;
 * everything is reported unlimited and setrlimit is a no-op (no resource accounting yet). struct
 * rlimit MUST match the libaios getrlimit/setrlimit signature (two rlim_t). */
#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H
#include <sys/types.h>

typedef unsigned long long rlim_t;
#define RLIM_INFINITY (~0ULL)

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_RSS     5
#define RLIMIT_NPROC   6
#define RLIMIT_NOFILE  7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS      9

int getrlimit(int resource, struct rlimit *rl);
int setrlimit(int resource, const struct rlimit *rl);

#endif /* _SYS_RESOURCE_H */

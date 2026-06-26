/* getopt.h -- AIOS shadow header. getopt (+ its globals) lives in <unistd.h>; this adds the GNU
 * getopt_long surface dash's histedit.c includes. getopt_long ignores long options (falls back to
 * short-option getopt) -- enough for the SMALL build. */
#ifndef _GETOPT_H
#define _GETOPT_H
#include <unistd.h>     /* getopt + optarg/optind/opterr/optopt */

struct option {
    const char *name;
    int         has_arg;
    int        *flag;
    int         val;
};
#define no_argument       0
#define required_argument 1
#define optional_argument 2

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

#endif /* _GETOPT_H */

/* nice — run a command at adjusted priority */
#include <stdio.h>
#include "aios_posix.h"

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    if (argc < 4) {
        printf("usage: nice <n> <command> [args...]\n");
        printf("  n: -20 (highest) to 19 (lowest), default 0\n");
        return 1;
    }
    /* Parse nice value */
    int nice = 0;
    const char *s = argv[2];
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { nice = nice * 10 + (*s - '0'); s++; }
    if (neg) nice = -nice;

    printf("nice: would run '%s' at nice %d (priority %d)\n",
           argv[3], nice, 200 - nice);
    printf("(full nice exec not yet implemented — use ps to see current priorities)\n");
    return 0;
}

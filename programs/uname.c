#include "aios.h"
#include "posix.h"

AIOS_ENTRY {
    struct utsname u;
    uname(&u);

    const char *args = posix_args();
    int flag_a = 0, flag_s = 0, flag_n = 0, flag_r = 0, flag_v = 0, flag_m = 0;
    int has_flags = 0;

    /* Parse args string for flags */
    if (args) {
        const char *p = args;
        while (*p) {
            if (*p == '-') {
                p++;
                while (*p && *p != ' ') {
                    if (*p == 'a') flag_a = 1;
                    else if (*p == 's') flag_s = 1;
                    else if (*p == 'n') flag_n = 1;
                    else if (*p == 'r') flag_r = 1;
                    else if (*p == 'v') flag_v = 1;
                    else if (*p == 'm') flag_m = 1;
                    has_flags = 1;
                    p++;
                }
            } else {
                p++;
            }
        }
    }

    if (!has_flags) flag_s = 1;
    if (flag_a) { flag_s = flag_n = flag_r = flag_v = flag_m = 1; }

    int first = 1;
    if (flag_s) { if (!first) write(1, " ", 1); write(1, u.sysname, strlen(u.sysname)); first = 0; }
    if (flag_n) { if (!first) write(1, " ", 1); write(1, u.nodename, strlen(u.nodename)); first = 0; }
    if (flag_r) { if (!first) write(1, " ", 1); write(1, u.release, strlen(u.release)); first = 0; }
    if (flag_v) { if (!first) write(1, " ", 1); write(1, u.version, strlen(u.version)); first = 0; }
    if (flag_m) { if (!first) write(1, " ", 1); write(1, u.machine, strlen(u.machine)); first = 0; }
    write(1, "\n", 1);
    return 0;
}

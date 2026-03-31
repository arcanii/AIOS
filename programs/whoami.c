#include "aios.h"
#include "posix.h"

AIOS_ENTRY {
    uid_t uid = getuid();
    struct passwd *pw = getpwuid(uid);
    if (pw) {
        write(STDOUT_FILENO, pw->pw_name, strlen(pw->pw_name));
    } else {
        char buf[12];
        int i = 0;
        unsigned u = (unsigned)uid;
        if (u == 0) { buf[i++] = '0'; }
        else { char tmp[12]; int t = 0; while (u) { tmp[t++] = '0' + (u % 10); u /= 10; } while (t--) buf[i++] = tmp[t]; }
        write(STDOUT_FILENO, buf, (size_t)i);
    }
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}

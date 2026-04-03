/* POSIX uname — queries AIOS kernel */
#include <stdio.h>
#include <sys/utsname.h>

int main(int argc, char *argv[]) {
    struct utsname uts;
    if (uname(&uts) != 0) {
        fprintf(stderr, "uname: failed\n");
        return 1;
    }
    printf("%s %s %s %s %s\n",
        uts.sysname, uts.nodename, uts.release, uts.version, uts.machine);
    return 0;
}

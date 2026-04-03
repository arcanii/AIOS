#include <stdio.h>
#include "aios_posix.h"
int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    printf("AIOS miniShell\n");
    printf("Builtins: cd <dir>, exit\n");
    printf("Programs: ls, cat, wc, head, echo, uname, ps, grep, sort, mkdir,\n");
    printf("  touch, rm, id, whoami, date, env, basename, dirname, pwd,\n");
    printf("  sysinfo, posix_test, gnu_hello\n");
    return 0;
}

#include <stdio.h>
#include "aios_posix.h"
int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    printf("HOME=/\n");
    printf("PATH=/bin\n");
    printf("USER=root\n");
    printf("SHELL=/bin/sh\n");
    printf("TERM=vt100\n");
    return 0;
}

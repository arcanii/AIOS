/* POSIX uname — pure standard C */
#include <stdio.h>
#include "aios_posix.h"

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    printf("AIOS 0.4.x aarch64 seL4 15.0.0 Cortex-A53 4-SMP\n");
    return 0;
}

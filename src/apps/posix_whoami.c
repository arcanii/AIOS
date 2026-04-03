#include <stdio.h>
#include "aios_posix.h"
int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    printf("root\n");
    return 0;
}

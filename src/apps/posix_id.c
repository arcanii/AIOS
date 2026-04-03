#include <stdio.h>
#include <unistd.h>
#include "aios_posix.h"
int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    printf("uid=%d(root) gid=%d(root)\n", getuid(), getgid());
    return 0;
}

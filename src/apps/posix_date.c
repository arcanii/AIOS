#include <stdio.h>
#include <time.h>
#include "aios_posix.h"
int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    struct timespec ts;
    clock_gettime(0, &ts);
    printf("uptime: %ld.%03ld seconds\n", ts.tv_sec, ts.tv_nsec / 1000000);
    return 0;
}

/* tee — read stdin, write to stdout and file */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "aios_posix.h"
int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    /* For now, just copy stdin to stdout (file write via IPC not yet) */
    char buf[512];
    int n;
    while ((n = read(0, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    return 0;
}

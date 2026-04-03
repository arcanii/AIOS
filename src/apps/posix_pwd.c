#include <stdio.h>
#include <unistd.h>
#include "aios_posix.h"
int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    char buf[256];
    if (getcwd(buf, sizeof(buf))) printf("%s\n", buf);
    return 0;
}

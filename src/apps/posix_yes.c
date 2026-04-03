#include <stdio.h>
#include "aios_posix.h"
int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    const char *msg = (argc > 2) ? argv[2] : "y";
    for (int i = 0; i < 100; i++) printf("%s\n", msg);
    return 0;
}

#include <stdio.h>
#include "aios_posix.h"
int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    if (argc < 3) { fprintf(stderr, "usage: basename <path>\n"); return 1; }
    const char *p = argv[2];
    const char *last = p;
    while (*p) { if (*p == '/') last = p + 1; p++; }
    printf("%s\n", last);
    return 0;
}

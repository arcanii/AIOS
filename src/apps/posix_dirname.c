#include <stdio.h>
#include "aios_posix.h"
int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    if (argc < 3) { fprintf(stderr, "usage: dirname <path>\n"); return 1; }
    char buf[256];
    const char *s = argv[2];
    int i = 0;
    while (s[i] && i < 255) { buf[i] = s[i]; i++; }
    buf[i] = '\0';
    while (i > 1 && buf[i-1] != '/') i--;
    if (i > 1) i--;
    buf[i] = '\0';
    printf("%s\n", buf[0] ? buf : "/");
    return 0;
}

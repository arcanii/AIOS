/*
 * prog_args.c -- a normal-looking C program (main + printf + malloc + argv), compiled against
 * libaios (the AIOS-ABI C runtime), NOT the system libc. Running it on the AIOS userspace kernel
 * proves the libaios model: write ordinary C, link libaios, run as an AIOS program. This is the
 * pattern sbase/dash will follow once libaios grows into a full libc.
 */
#include "libaios.h"

int main(int argc, char **argv) {
    printf("hello from a libaios C program running on the AIOS userspace kernel\n");
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = %s\n", i, argv[i]);

    char *buf = malloc(64);
    if (buf) {
        for (int i = 0; i < 26; i++) buf[i] = (char)('a' + i);
        buf[26] = '\0';
        printf("malloc + write-back: %s\n", buf);
    }

    printf("hex/char check: 0x%x %c%c\n", 3735928559u, 'O', 'K');
    return 0;
}

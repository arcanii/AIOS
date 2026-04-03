/*
 * AIOS 0.4.x — cat (POSIX program)
 *
 * argv[0] = serial_ep, argv[1] = fs_ep, argv[2..] = filenames
 *
 * Uses aios_posix shim for printf.
 * Reads files via direct IPC to fs_thread.
 */
#include <stdio.h>
#include <stdint.h>
#include <sel4/sel4.h>
#include "aios_posix.h"

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void cat_file(const char *path) {
    seL4_CPtr fs = aios_get_fs_ep();
    if (!fs) {
        printf("cat: no filesystem\n");
        return;
    }

    /* Pack path into MRs */
    int pl = str_len(path);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }

    seL4_MessageInfo_t reply = seL4_Call(fs,
        seL4_MessageInfo_new(AIOS_FS_CAT, 0, 0, mr));
    seL4_Word total = seL4_GetMR(0);

    if (total == 0) {
        printf("cat: %s: No such file\n", path);
        return;
    }

    /* Unpack and print */
    int mrs = (int)seL4_MessageInfo_get_length(reply) - 1;
    for (int i = 0; i < mrs; i++) {
        seL4_Word rw = seL4_GetMR(i + 1);
        for (int j = 0; j < 8 && (int)(i * 8 + j) < (int)total; j++) {
            char c = (char)((rw >> (j * 8)) & 0xFF);
            printf("%c", c);
        }
    }
}

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);

    if (argc < 3) {
        printf("Usage: cat <file> [file2 ...]\n");
        return 1;
    }

    for (int i = 2; i < argc; i++) {
        cat_file(argv[i]);
    }
    return 0;
}

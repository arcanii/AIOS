/*
 * POSIX ls — uses stat() to show file type and size
 * Falls back to FS_LS IPC for directory listing
 */
#include <stdio.h>
#include <stdint.h>
#include <sel4/sel4.h>
#include "aios_posix.h"

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);

    seL4_CPtr fs = aios_get_fs_ep();
    if (!fs) { printf("ls: no filesystem\n"); return 1; }

    const char *path = (argc > 2) ? argv[2] : "/";

    /* Send FS_LS to get directory listing */
    int pl = str_len(path);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }

    seL4_MessageInfo_t reply = seL4_Call(fs,
        seL4_MessageInfo_new(AIOS_FS_LS, 0, 0, mr));
    seL4_Word total = seL4_GetMR(0);

    if (total == 0) {
        printf("ls: cannot access '%s'\n", path);
        return 1;
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
    return 0;
}

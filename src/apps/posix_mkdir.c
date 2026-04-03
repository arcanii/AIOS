#include <stdio.h>
#include <stdint.h>
#include <sel4/sel4.h>
#include "aios_posix.h"
int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    if (argc < 3) { fprintf(stderr, "usage: mkdir <dir>\n"); return 1; }
    seL4_CPtr fs = aios_get_fs_ep();
    if (!fs) { fprintf(stderr, "mkdir: no filesystem\n"); return 1; }
    const char *path = argv[2];
    int pl = 0; while (path[pl]) pl++;
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1; seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(fs, seL4_MessageInfo_new(14, 0, 0, mr));
    if ((int)(long)seL4_GetMR(0) != 0) { fprintf(stderr, "mkdir: failed\n"); return 1; }
    return 0;
}

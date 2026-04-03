/*
 * POSIX ls — uses opendir/readdir (real POSIX, no raw IPC)
 */
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include "aios_posix.h"

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);

    const char *path = (argc > 2) ? argv[2] : "/";

    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "ls: cannot open '%s'\n", path);
        return 1;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        char type = '?';
        if (de->d_type == 4) type = 'd';      /* DT_DIR */
        else if (de->d_type == 8) type = '-';  /* DT_REG */
        printf("%c %s\n", type, de->d_name);
    }
    closedir(dir);
    return 0;
}

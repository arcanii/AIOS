/* POSIX echo — pure standard C */
#include <stdio.h>
#include <unistd.h>
#include "aios_posix.h"

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    for (int i = 2; i < argc; i++) {
        if (i > 2) write(1, " ", 1);
        int len = 0;
        while (argv[i][len]) len++;
        write(1, argv[i], len);
    }
    write(1, "\n", 1);
    return 0;
}

#include "aios.h"
#include "posix.h"
AIOS_ENTRY {
    const char *filename = posix_args();
    if (!filename || !filename[0]) {
        write(STDERR_FILENO, "usage: stat <file>\n", 19);
        return 1;
    }
    struct stat st;
    if (stat(filename, &st) < 0) {
        write(STDERR_FILENO, filename, strlen(filename));
        write(STDERR_FILENO, ": not found\n", 12);
        return 1;
    }
    write(STDOUT_FILENO, "  File: ", 8);
    write(STDOUT_FILENO, filename, strlen(filename));
    write(STDOUT_FILENO, "\n  Size: ", 9);
    char num[20]; int ni = 0;
    unsigned long sz = (unsigned long)st.st_size;
    if (sz == 0) num[ni++] = '0';
    else while (sz) { num[ni++] = '0' + (char)(sz % 10); sz /= 10; }
    for (int j = ni - 1; j >= 0; j--) write(STDOUT_FILENO, &num[j], 1);
    write(STDOUT_FILENO, " bytes\n  Type: regular file\n", 28);
    return 0;
}

#include "aios.h"
#include "posix.h"
AIOS_ENTRY {
    DIR *d = opendir("/");
    if (!d) {
        write(STDERR_FILENO, "ls: cannot open directory\n", 25);
        return 1;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* Print name */
        int len = 0;
        while (ent->d_name[len]) len++;
        write(STDOUT_FILENO, ent->d_name, (size_t)len);

        /* Print size */
        write(STDOUT_FILENO, "\t", 1);
        unsigned long sz = ent->d_size;
        char num[12];
        int ni = 0;
        if (sz == 0) { num[ni++] = '0'; }
        else { while (sz) { num[ni++] = '0' + (char)(sz % 10); sz /= 10; } }
        /* Reverse */
        for (int j = 0; j < ni / 2; j++) {
            char t = num[j]; num[j] = num[ni-1-j]; num[ni-1-j] = t;
        }
        write(STDOUT_FILENO, num, (size_t)ni);
        write(STDOUT_FILENO, "\n", 1);
    }
    closedir(d);
    return 0;
}

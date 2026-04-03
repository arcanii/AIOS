/* POSIX env — print environment variables */
#include <stdio.h>
#include <stdlib.h>

extern char **environ;

int main(int argc, char *argv[]) {
    if (environ) {
        for (char **e = environ; *e; e++)
            printf("%s\n", *e);
    }
    return 0;
}

/*
 * prog_libc.c -- a real-libc-style C program (M3e step 1: the libc retarget begins).
 *
 * It includes ONLY standard headers and calls ONLY standard library functions -- no aios_abi.h, no
 * libaios.h, no aios_* names. Compiled with -nostdinc against AIOS's shadow headers (uk/lib/include)
 * and linked against libaios, it proves the path that will recompile real `sbase`/`dash` sources
 * unmodified: ordinary C sees a POSIX-ish libc that the AIOS kernel implements behind its ABI.
 *
 * Exercises stdlib (strtol/strdup/getenv/malloc/free), string + ctype, file I/O (open/read/close),
 * and the process model (fork/execv/waitpid) -- all via <stdio.h>/<stdlib.h>/<string.h>/<unistd.h>/
 * <fcntl.h>/<sys/wait.h>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
    printf("prog_libc: real C, standard headers only, compiled -nostdinc\n");

    char *end;
    long v = strtol("  -1234rest", &end, 10);
    printf("  strtol -> %ld, leftover \"%s\", toupper('a')=%c\n", v, end, (char)toupper('a'));

    char *dup = strdup("hello,world");
    char *comma = strchr(dup, ',');
    if (comma) *comma = ' ';
    printf("  strdup+strchr -> \"%s\", strstr(world) -> \"%s\"\n", dup, strstr(dup, "world"));
    free(dup);

    char *home = getenv("HOME");
    printf("  getenv(HOME) -> %s\n", home ? home : "(unset)");

    const char *path = (argc >= 2) ? argv[1] : "/etc/hostname";
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { printf("  open(%s) failed\n", path); return 1; }
    char *buf = malloc(4096);
    long lines = 0, bytes = 0, n;
    while ((n = read(fd, buf, 4096)) > 0)
        for (long i = 0; i < n; i++) { bytes++; if (buf[i] == '\n') lines++; }
    close(fd);
    free(buf);
    printf("  read %s -> %ld lines, %ld bytes\n", path, lines, bytes);

    pid_t pid = fork();
    if (pid == 0) {
        char *a[] = { "./prog_args", "from-libc", NULL };
        execv("./prog_args", a);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    printf("  forked ./prog_args, it exited %d\n", WEXITSTATUS(status));
    return 0;
}

/*
 * prog_umask.c -- proves the file-creation mask is a real PER-PROCESS umask (kernel-owned, applied
 * on create), not the old no-op tracker: a 0666 create under umask 077 becomes 0600, a 0777 mkdir
 * becomes 0700, and the mask is inherited across fork without a child's change leaking to the parent.
 * Plain C via the shadow headers. Exits 0 iff all checks pass.
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

static unsigned int mode_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (st.st_mode & 0777) : 0xFFFFu;
}

int main(void) {
    int fails = 0;

    unsigned int old = umask(077);
    printf("umask(077) -> previous %03o (expect 022)\n", old);
    if (old != 022) { printf("  FAIL: default umask was not 022\n"); fails++; }

    unlink("/tmp/aios_um.txt");
    int fd = open("/tmp/aios_um.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) close(fd);
    unsigned int fm = mode_of("/tmp/aios_um.txt");
    printf("create 0666 under umask 077 -> %03o (expect 600)\n", fm);
    if (fm != 0600) { printf("  FAIL\n"); fails++; }
    unlink("/tmp/aios_um.txt");

    rmdir("/tmp/aios_um_d");
    mkdir("/tmp/aios_um_d", 0777);
    unsigned int dm = mode_of("/tmp/aios_um_d");
    printf("mkdir 0777 under umask 077 -> %03o (expect 700)\n", dm);
    if (dm != 0700) { printf("  FAIL\n"); fails++; }
    rmdir("/tmp/aios_um_d");

    /* per-process: a child inherits the mask; its change does not leak back */
    long pid = fork();
    if (pid == 0) { unsigned int cold = umask(022); _exit(cold == 077 ? 7 : 8); }   /* child sees 077 */
    int st; waitpid(pid, &st, 0);
    if (WEXITSTATUS(st) != 7) { printf("  FAIL: child did not inherit umask 077 across fork\n"); fails++; }
    else                        printf("  ok: child inherited umask 077 across fork\n");

    unsigned int now = umask(022);                    /* parent still 077 (child change did not leak) */
    if (now != 077) { printf("  FAIL: a child umask change leaked to the parent (%03o)\n", now); fails++; }
    else            printf("  ok: parent umask unaffected by the child\n");

    printf("prog_umask: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

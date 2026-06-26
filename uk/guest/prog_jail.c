/*
 * prog_jail.c -- M4.2 red-team + positive proof of FILESYSTEM CONFINEMENT (the other half of the
 * boundary). Run under AIOS_ROOT, the kernel/PAL confines every file path to that root via
 * openat2(RESOLVE_IN_ROOT). This program asserts the confinement holds BOTH ways:
 *   - positive: in-root files are reachable; in-root create/mkdir/rename/unlink/rmdir/readdir work;
 *   - red-team: every escape vector is DENIED -- an absolute host path, a ".." traversal, a symlink
 *     with an absolute out-of-root target, and a symlink with a ".." out-of-root target.
 * Plain C through the shadow headers (the same path that recompiles sbase/dash). Exits 0 iff every
 * check passes -- the kernel's run.sh sets up the sandbox + the out-of-root secret and gates on it.
 *
 * This is to M4.2 what guest_escape.c is to M4: a concrete adversary proving the boundary by
 * failing to cross it.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static int fails = 0;

/* Expect open(path) to FAIL -- the guest is confined and must not reach this path. */
static void must_block(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd >= 0) { printf("  FAIL  open(%s) SUCCEEDED -- escaped the AIOS root!\n", path); close(fd); fails++; }
    else         printf("  ok    open(%s) denied (%s)\n", path, strerror(errno));
}

/* Expect open(path) to SUCCEED and its contents to contain `want`. */
static void must_read(const char *path, const char *want) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("  FAIL  open(%s) failed (%s) -- in-root read should work\n", path, strerror(errno)); fails++; return; }
    char buf[128]; long n = read(fd, buf, (long)sizeof buf - 1); close(fd);
    if (n < 0) { printf("  FAIL  read(%s) failed\n", path); fails++; return; }
    buf[n] = '\0';
    if (strstr(buf, want)) printf("  ok    open+read(%s) -> contains \"%s\"\n", path, want);
    else { printf("  FAIL  open+read(%s) -> \"%s\" (wanted \"%s\")\n", path, buf, want); fails++; }
}

int main(void) {
    char cwd[256];
    printf("prog_jail: proving the guest is confined to its AIOS root\n");

    /* --- positive: in-root files are reachable --- */
    must_read("/inside.txt",   "visible");
    must_read("/sub/deep.txt", "deep");

    /* --- red-team: every escape vector must be denied --- */
    must_block("/etc/passwd");                 /* absolute host path                 */
    must_block("/etc/hostname");
    must_block("../aios_jail_secret.txt");     /* ".." traversal above the root      */
    must_block("/../aios_jail_secret.txt");
    must_block("/abs_link");                   /* symlink: absolute out-of-root target */
    must_block("/dotdot_link");                /* symlink: ".." out-of-root target     */

    /* stat must not leak host metadata either (resolution is confined for stat too) */
    struct stat st;
    if (stat("/etc/passwd", &st) == 0) { printf("  FAIL  stat(/etc/passwd) leaked host metadata\n"); fails++; }
    else                                 printf("  ok    stat(/etc/passwd) denied (%s)\n", strerror(errno));

    /* --- the logical cwd starts at the root; chdir + a relative open resolve within the root --- */
    if (getcwd(cwd, sizeof cwd) && strcmp(cwd, "/") == 0) printf("  ok    getcwd -> \"/\"\n");
    else { printf("  FAIL  getcwd -> \"%s\" (wanted \"/\")\n", cwd); fails++; }
    if (chdir("/sub") == 0) {
        if (getcwd(cwd, sizeof cwd) && strcmp(cwd, "/sub") == 0) printf("  ok    chdir(/sub); getcwd -> \"/sub\"\n");
        else { printf("  FAIL  after chdir(/sub), getcwd -> \"%s\"\n", cwd); fails++; }
        must_read("deep.txt", "deep");          /* relative -> resolves in /sub */
    } else { printf("  FAIL  chdir(/sub): %s\n", strerror(errno)); fails++; }

    /* ".." at the root is clamped to the root -- the guest cannot climb out via cwd either */
    chdir("/");
    if (chdir("..") == 0 && getcwd(cwd, sizeof cwd) && strcmp(cwd, "/") == 0)
        printf("  ok    chdir(\"..\") at root stays at \"/\"\n");
    else { printf("  FAIL  chdir(\"..\") at root: cwd is now \"%s\"\n", cwd); fails++; }

    /* --- name ops (mkdir/create/rename/unlink/rmdir) all work WITHIN the root --- */
    if (mkdir("/jdir", 0755) == 0) {
        int f = open("/jdir/a", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f >= 0) { write(f, "x", 1); close(f); }
        if (f >= 0 && rename("/jdir/a", "/jdir/b") == 0 && stat("/jdir/b", &st) == 0 &&
            unlink("/jdir/b") == 0 && rmdir("/jdir") == 0)
            printf("  ok    mkdir/create/rename/unlink/rmdir within the root\n");
        else { printf("  FAIL  in-root name ops: %s\n", strerror(errno)); fails++; }
    } else { printf("  FAIL  mkdir(/jdir): %s\n", strerror(errno)); fails++; }

    /* reading a symlink's TEXT is allowed (it is not following it out of the root) */
    char lbuf[256];
    long ln = readlink("/abs_link", lbuf, sizeof lbuf - 1);
    if (ln > 0) { lbuf[ln] = '\0'; printf("  ok    readlink(/abs_link) -> \"%s\" (link text only)\n", lbuf); }
    else { printf("  FAIL  readlink(/abs_link): %s\n", strerror(errno)); fails++; }

    /* readdir of the root lists in-root names (opendir/getdents go through the confined open) */
    DIR *d = opendir("/");
    if (d) {
        struct dirent *de; int seen = 0;
        while ((de = readdir(d))) if (strcmp(de->d_name, "inside.txt") == 0) seen = 1;
        closedir(d);
        if (seen) printf("  ok    readdir(\"/\") shows the root (inside.txt present)\n");
        else { printf("  FAIL  readdir(\"/\") did not list inside.txt\n"); fails++; }
    } else { printf("  FAIL  opendir(\"/\"): %s\n", strerror(errno)); fails++; }

    printf("prog_jail: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

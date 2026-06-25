/*
 * prog_fs.c -- exercises the filesystem namespace syscalls (M3e step 4): mkdir, stat/lstat,
 * rename, getcwd, chdir, unlink, rmdir, getpid. Real C via the -nostdinc shadow headers. This is
 * what sbase's mkdir/rm/mv/pwd/cd (and ls's stat half) need.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

int main(void) {
    int fails = 0;
    const char *dir = "/tmp/aios_fs_test";
    const char *f1  = "/tmp/aios_fs_test/a.txt";
    const char *f2  = "/tmp/aios_fs_test/b.txt";

    unlink(f1); unlink(f2); rmdir(dir);          /* clean slate (ignore errors) */

    if (mkdir(dir, 0755) != 0) { printf("mkdir: %s\n", strerror(errno)); fails++; }
    struct stat st;
    if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) printf("mkdir+stat: %s is a dir (mode 0%o)\n", dir, st.st_mode & 0777);
    else { printf("stat(dir): not a directory\n"); fails++; }

    FILE *fp = fopen(f1, "w");
    if (fp) { fputs("hello stat\n", fp); fclose(fp); } else { printf("fopen: %s\n", strerror(errno)); fails++; }
    if (stat(f1, &st) == 0 && S_ISREG(st.st_mode)) printf("stat file: size=%lld bytes, regular file\n", st.st_size);
    else { printf("stat(file) failed\n"); fails++; }

    if (rename(f1, f2) != 0) { printf("rename: %s\n", strerror(errno)); fails++; }
    if (stat(f1, &st) == 0)        { printf("old name still exists!\n"); fails++; }
    else if (errno != ENOENT)      { printf("rename: expected ENOENT, got %s\n", strerror(errno)); fails++; }
    if (stat(f2, &st) == 0) printf("rename: a.txt -> b.txt OK\n"); else { printf("new name missing\n"); fails++; }

    char cwd[256], cwd2[256];
    if (getcwd(cwd, sizeof cwd)) printf("getcwd: %s\n", cwd); else { printf("getcwd failed\n"); fails++; }
    if (chdir(dir) != 0) { printf("chdir: %s\n", strerror(errno)); fails++; }
    if (getcwd(cwd2, sizeof cwd2) && strcmp(cwd2, dir) == 0) printf("chdir: now in %s\n", cwd2);
    else { printf("chdir/getcwd mismatch\n"); fails++; }
    chdir(cwd);                                  /* restore */

    if (unlink(f2) != 0) { printf("unlink: %s\n", strerror(errno)); fails++; }
    if (rmdir(dir)  != 0) { printf("rmdir: %s\n", strerror(errno)); fails++; }
    if (stat(dir, &st) == 0) { printf("dir survived rmdir!\n"); fails++; }

    printf("getpid() = %d\n", getpid());
    printf("prog_fs: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

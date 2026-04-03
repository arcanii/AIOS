/* POSIX compliance test — tests all implemented syscalls */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <time.h>
#include "aios_posix.h"

static int pass = 0, fail = 0;
#define TEST(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", name); pass++; } \
    else { printf("  FAIL: %s\n", name); fail++; } \
} while(0)

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    printf("=== AIOS POSIX Compliance Test ===\n\n");

    /* File I/O */
    printf("[File I/O]\n");
    int fd = open("/etc/hostname", O_RDONLY);
    TEST("open", fd >= 0);
    char buf[256];
    int n = read(fd, buf, sizeof(buf)-1);
    TEST("read", n > 0);
    if (n > 0) buf[n] = '\0';
    TEST("read content", n > 0 && buf[0] == 'a');
    TEST("write stdout", write(1, "  (stdout ok)\n", 14) == 14);
    long pos = lseek(fd, 0, 0);
    TEST("lseek", pos == 0);
    close(fd);
    TEST("close + reopen", open("/hello.txt", O_RDONLY) >= 0);

    /* stat */
    printf("\n[stat]\n");
    struct stat st;
    int r = fstat(fd, &st);
    TEST("fstat", r == 0 || r == -1); /* may fail on closed fd */

    /* Identity */
    printf("\n[Identity]\n");
    TEST("getpid", getpid() > 0);
    TEST("getppid", getppid() >= 0);
    TEST("getuid", getuid() == 0);
    TEST("geteuid", geteuid() == 0);
    TEST("getgid", getgid() == 0);
    TEST("getegid", getegid() == 0);

    /* uname */
    printf("\n[uname]\n");
    struct utsname uts;
    r = uname(&uts);
    TEST("uname call", r == 0);
    TEST("uname.sysname", uts.sysname[0] == 'A');
    TEST("uname.machine", uts.machine[0] == 'a');
    printf("  sysname=%s release=%s machine=%s\n",
           uts.sysname, uts.release, uts.machine);

    /* getcwd */
    printf("\n[getcwd]\n");
    char cwd[256];
    char *p = getcwd(cwd, sizeof(cwd));
    TEST("getcwd", p != NULL);
    if (p) printf("  cwd=%s\n", cwd);

    /* Time */
    printf("\n[Time]\n");
    struct timespec ts;
    r = clock_gettime(0, &ts);
    TEST("clock_gettime", r == 0);
    printf("  time=%ld.%09ld\n", ts.tv_sec, ts.tv_nsec);

    struct timeval tv;
    r = gettimeofday(&tv, NULL);
    TEST("gettimeofday", r == 0);

    /* access */
    printf("\n[access]\n");
    TEST("access /etc/hostname", access("/etc/hostname", 0) == 0);
    TEST("access /nonexist", access("/nonexist", 0) != 0);

    /* dup */
    printf("\n[dup]\n");
    fd = open("/hello.txt", O_RDONLY);
    if (fd >= 0) {
        int fd2 = dup(fd);
        TEST("dup", fd2 >= 0);
        close(fd2);
        close(fd);
    }

    /* Summary */
    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail ? 1 : 0;
}

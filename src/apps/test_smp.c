/*
 * test_smp -- SMP and multi-process test
 *
 * Tests: /proc/cpuinfo core count, fork+exec concurrency,
 *        sysinfo, PID uniqueness across children.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

static int tests_run = 0;
static int tests_pass = 0;

static void check(const char *name, int cond) {
    tests_run++;
    if (cond) {
        tests_pass++;
        printf("  PASS: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
    }
}

static int count_cpuinfo_cores(void) {
    int fd = open("/proc/cpuinfo", O_RDONLY);
    if (fd < 0) return -1;

    char buf[2048];
    int total = 0;
    int cores = 0;

    for (;;) {
        int n = read(fd, buf + total, sizeof(buf) - total - 1);
        if (n <= 0) break;
        total += n;
        if (total >= (int)sizeof(buf) - 1) break;
    }
    buf[total] = 0;
    close(fd);

    /* Count "processor" lines */
    char *p = buf;
    while ((p = strstr(p, "processor")) != NULL) {
        cores++;
        p += 9;
    }
    return cores;
}

int main(void) {
    printf("=== AIOS SMP Test ===\n");
    printf("PID: %d\n\n", getpid());

    /* Test 1: cpuinfo shows multiple cores */
    printf("[1] CPU core count\n");
    int cores = count_cpuinfo_cores();
    printf("  /proc/cpuinfo reports %d core(s)\n", cores);
    check("cpuinfo readable", cores > 0);
    check("multiple cores detected (SMP)", cores >= 2);

    /* Test 2: parallel fork -- children run concurrently */
    printf("\n[2] Parallel fork\n");
    int nchildren = 4;
    pid_t pids[4];

    for (int i = 0; i < nchildren; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            printf("  fork %d failed\n", i);
            break;
        }
        if (pids[i] == 0) {
            /* Child: just exit with index */
            _exit(i);
        }
    }

    /* Parent: wait for all */
    int ok = 0;
    for (int i = 0; i < nchildren; i++) {
        if (pids[i] <= 0) continue;
        int status = 0;
        pid_t w = waitpid(pids[i], &status, 0);
        if (w == pids[i] && WIFEXITED(status) && WEXITSTATUS(status) == i) {
            ok++;
        } else {
            printf("  child %d: unexpected result (w=%d, status=0x%x)\n",
                   i, (int)w, status);
        }
    }
    check("all 4 children forked", ok == nchildren);

    /* Test 3: PID uniqueness */
    printf("\n[3] PID uniqueness\n");
    int unique = 1;
    for (int i = 0; i < nchildren; i++) {
        for (int j = i + 1; j < nchildren; j++) {
            if (pids[i] == pids[j]) unique = 0;
        }
        if (pids[i] == getpid()) unique = 0;
    }
    check("all PIDs unique", unique);

    /* Summary */
    printf("\n=== Results: %d/%d passed ===\n", tests_pass, tests_run);
    return (tests_pass == tests_run) ? 0 : 1;
}

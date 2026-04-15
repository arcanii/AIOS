/*
 * test_hw -- Hardware identification and validation test
 *
 * Validates that the DTB parser correctly populated hardware info
 * by reading /proc entries and checking for sane values.
 * Reports platform-specific features (EMMC, GENET, VC mailbox).
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

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

static int read_proc(const char *path, char *buf, int bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int total = 0;
    for (;;) {
        int n = read(fd, buf + total, bufsz - total - 1);
        if (n <= 0) break;
        total += n;
        if (total >= bufsz - 1) break;
    }
    buf[total] = 0;
    close(fd);
    return total;
}

/* Parse "MemTotal:    NNNN kB" and return kB value */
static unsigned long parse_memtotal(const char *buf) {
    const char *p = strstr(buf, "MemTotal:");
    if (!p) return 0;
    p += 9;
    while (*p == ' ' || *p == '\t') p++;
    unsigned long val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return val;
}

static int count_cores(const char *buf) {
    int cores = 0;
    const char *p = buf;
    while ((p = strstr(p, "processor")) != NULL) {
        cores++;
        p += 9;
    }
    return cores;
}

int main(void) {
    char buf[4096];
    int n;

    printf("=== AIOS Hardware Validation Test ===\n\n");

    /* Test 1: /proc/version readable and contains AIOS */
    printf("[1] System identification\n");
    n = read_proc("/proc/version", buf, sizeof(buf));
    check("/proc/version readable", n > 0);
    check("contains AIOS", n > 0 && strstr(buf, "AIOS") != NULL);
    check("contains seL4", n > 0 && strstr(buf, "seL4") != NULL);
    if (n > 0) {
        printf("  version: ");
        for (int i = 0; i < n && buf[i] != '\n'; i++) putchar(buf[i]);
        printf("\n");
    }

    /* Test 2: /proc/cpuinfo has valid core count */
    printf("\n[2] CPU detection\n");
    n = read_proc("/proc/cpuinfo", buf, sizeof(buf));
    check("/proc/cpuinfo readable", n > 0);
    int cores = (n > 0) ? count_cores(buf) : 0;
    printf("  detected %d core(s)\n", cores);
    check("at least 1 core", cores >= 1);
    check("core count <= 8 (sane)", cores <= 8);

    /* Detect CPU model */
    int is_a72 = (n > 0 && strstr(buf, "cortex-a72") != NULL);
    int is_a53 = (n > 0 && strstr(buf, "cortex-a53") != NULL);
    check("CPU model identified", is_a72 || is_a53);
    if (is_a72) printf("  platform: RPi4 (Cortex-A72)\n");
    if (is_a53) printf("  platform: QEMU virt (Cortex-A53)\n");

    /* Test 3: Memory detection */
    printf("\n[3] Memory detection\n");
    n = read_proc("/proc/meminfo", buf, sizeof(buf));
    check("/proc/meminfo readable", n > 0);
    unsigned long mem_kb = (n > 0) ? parse_memtotal(buf) : 0;
    printf("  MemTotal: %lu kB (%lu MB)\n", mem_kb, mem_kb / 1024);
    check("memory > 64 MB", mem_kb > 65536);
    check("memory < 8192 MB (sane)", mem_kb < 8192UL * 1024);

    /* Test 4: Filesystem mounts */
    printf("\n[4] Filesystem mounts\n");
    n = read_proc("/proc/mounts", buf, sizeof(buf));
    check("/proc/mounts readable", n > 0);
    check("root ext2 mounted", n > 0 && strstr(buf, "/ ext2") != NULL);
    check("procfs mounted", n > 0 && strstr(buf, "/proc proc") != NULL);

    /* Test 5: Storage -- verify block device is functional */
    printf("\n[5] Storage read test\n");
    int fd = open("/etc/passwd", O_RDONLY);
    check("/etc/passwd readable (disk I/O)", fd >= 0);
    if (fd >= 0) {
        char pbuf[256];
        int pr = read(fd, pbuf, sizeof(pbuf) - 1);
        check("read returns data", pr > 0);
        if (pr > 0) {
            pbuf[pr] = 0;
            check("contains root user", strstr(pbuf, "root") != NULL);
        }
        close(fd);
    }

    /* Test 6: Uptime (timer working) */
    printf("\n[6] Timer / uptime\n");
    n = read_proc("/proc/uptime", buf, sizeof(buf));
    check("/proc/uptime readable", n > 0);
    if (n > 0) {
        printf("  uptime: ");
        for (int i = 0; i < n && buf[i] != '\n'; i++) putchar(buf[i]);
        printf("\n");
    }

    /* Test 7: Platform-specific features */
    printf("\n[7] Platform features\n");
    if (is_a72) {
        printf("  RPi4 expected features:\n");
        printf("    EMMC2 SD card: should be present\n");
        printf("    GENET Ethernet: should be present\n");
        printf("    VideoCore mailbox: should be present\n");
        printf("    Mini UART: serial console\n");
    } else if (is_a53) {
        printf("  QEMU expected features:\n");
        printf("    virtio-blk: storage\n");
        printf("    virtio-net: networking (if enabled)\n");
        printf("    PL011 UART: serial console\n");
        printf("    ramfb: display (if enabled)\n");
    }

    /* Summary */
    printf("\n=== Results: %d/%d passed ===\n", tests_pass, tests_run);
    return (tests_pass == tests_run) ? 0 : 1;
}

/*
 * hwinfo -- Dynamic hardware identification
 *
 * Reads /proc to display discovered hardware inventory.
 * Works on both QEMU and RPi4 -- reports what the DTB parser found.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

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

/* Extract value after "key: " or "key:\t" from a buffer line */
static const char *find_value(const char *buf, const char *key) {
    const char *p = strstr(buf, key);
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    return p;
}

static void print_line(const char *p) {
    while (*p && *p != '\n') {
        putchar(*p);
        p++;
    }
}

int main(void) {
    char buf[4096];
    int n;

    printf("=== AIOS Hardware Information ===\n\n");

    /* Platform identification */
    n = read_proc("/proc/version", buf, sizeof(buf));
    if (n > 0) {
        printf("System:    ");
        print_line(buf);
        printf("\n");
    }

    /* CPU info */
    n = read_proc("/proc/cpuinfo", buf, sizeof(buf));
    if (n > 0) {
        /* Count processors */
        int cores = 0;
        char *p = buf;
        const char *model = NULL;
        while ((p = strstr(p, "processor")) != NULL) {
            cores++;
            p += 9;
        }
        /* Get model name from first entry */
        const char *mn = find_value(buf, "model name");
        printf("CPU:       ");
        if (mn) print_line(mn);
        else printf("unknown");
        printf("\n");
        printf("Cores:     %d\n", cores);

        /* Detect platform from CPU compatible string */
        printf("Platform:  ");
        if (strstr(buf, "cortex-a72") || strstr(buf, "Cortex-A72"))
            printf("Raspberry Pi 4 (BCM2711)\n");
        else if (strstr(buf, "cortex-a76") || strstr(buf, "Cortex-A76"))
            printf("Raspberry Pi 5 (BCM2712)\n");
        else if (strstr(buf, "cortex-a53") || strstr(buf, "Cortex-A53"))
            printf("QEMU virt (Cortex-A53)\n");
        else
            printf("Unknown\n");
    }

    /* Memory */
    n = read_proc("/proc/meminfo", buf, sizeof(buf));
    if (n > 0) {
        printf("Memory:    ");
        const char *mt = find_value(buf, "MemTotal");
        if (mt) print_line(mt);
        else printf("unknown");
        printf("\n");
    }

    /* Mounts */
    printf("\n--- Storage ---\n");
    n = read_proc("/proc/mounts", buf, sizeof(buf));
    if (n > 0) {
        char *line = buf;
        while (*line) {
            printf("  ");
            while (*line && *line != '\n') {
                putchar(*line);
                line++;
            }
            printf("\n");
            if (*line == '\n') line++;
        }
    }

    /* Process summary */
    printf("\n--- Processes ---\n");
    n = read_proc("/proc/status", buf, sizeof(buf));
    if (n > 0) {
        int running = 0;
        int total = 0;
        char *p = buf;
        /* Count lines with state info */
        while (*p) {
            if (*p == '\n') {
                total++;
                p++;
            } else {
                p++;
            }
        }
        printf("  %d entries in process table\n", total);
    }

    /* Uptime */
    n = read_proc("/proc/uptime", buf, sizeof(buf));
    if (n > 0) {
        printf("\nUptime:    ");
        print_line(buf);
        printf("\n");
    }

    printf("\n=== End Hardware Info ===\n");
    return 0;
}

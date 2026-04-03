#include "aios/procfs.h"
#include "aios/aios_log.h"
#include <stdio.h>

proc_entry_t proc_table[PROC_MAX];
static int next_pid = 1;

void proc_init(void) {
    for (int i = 0; i < PROC_MAX; i++) proc_table[i].active = 0;
    /* PID 0: kernel/idle */
    proc_table[0].active = 1;
    proc_table[0].pid = 0;
    proc_table[0].priority = 255;
    proc_table[0].nice = -20;
    proc_table[0].name = "kernel";
    proc_table[0].state = 1;
}

int proc_add(const char *name, int priority) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (!proc_table[i].active) {
            proc_table[i].active = 1;
            proc_table[i].pid = next_pid++;
            proc_table[i].priority = priority;
            proc_table[i].nice = 0;
            proc_table[i].name = name;
            proc_table[i].state = 1;
            return proc_table[i].pid;
        }
    }
    return -1;
}

void proc_remove(int pid) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active && proc_table[i].pid == pid) {
            proc_table[i].active = 0;
            return;
        }
    }
}

/* Map nice (-20..19) to seL4 priority (220..181) */
int proc_get_priority(int nice) {
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    return 200 - nice;  /* nice 0 = 200, nice -20 = 220, nice 19 = 181 */
}

void proc_set_nice(int pid, int nice) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active && proc_table[i].pid == pid) {
            proc_table[i].nice = nice;
            proc_table[i].priority = proc_get_priority(nice);
            return;
        }
    }
}

/* ── procfs VFS operations ── */

static int procfs_list(void *ctx, uint32_t ino, char *buf, int bufsize) {
    (void)ctx; (void)ino;
    int w = 0;
    /* List virtual files */
    const char *entries[] = { "d .\n", "d ..\n", "- version\n", "- uptime\n", "- mounts\n", "- status\n", "- log\n" };
    for (int i = 0; i < 7 && w < bufsize - 1; i++) {
    
        const char *e = entries[i];
        while (*e && w < bufsize - 1) buf[w++] = *e++;
    }
    /* List process PIDs as directories */
    for (int i = 0; i < PROC_MAX && w < bufsize - 10; i++) {
        if (!proc_table[i].active) continue;
        buf[w++] = 'd'; buf[w++] = ' ';
        /* pid to string */
        char tmp[10]; int ti = 0;
        int v = proc_table[i].pid;
        if (v == 0) { tmp[ti++] = '0'; }
        else { while (v) { tmp[ti++] = '0' + v % 10; v /= 10; } }
        while (ti--) buf[w++] = tmp[ti];
        buf[w++] = '\n';
    }
    if (w < bufsize) buf[w] = '\0';
    return w;
}

static int procfs_read(void *ctx, const char *path, char *buf, int bufsize) {
    (void)ctx;
    if (path[0] == '/') path++;

    int w = 0;
    if (path[0] == 'v' && path[1] == 'e') {
        /* /proc/version */
        const char *ver = "AIOS 0.4.x (seL4 15.0.0, AArch64, 4-core SMP)\n";
        while (*ver && w < bufsize - 1) buf[w++] = *ver++;
    } else if (path[0] == 'm') {
        /* /proc/mounts */
        const char *mnt = "/dev/vda / ext2 ro 0 0\nproc /proc proc rw 0 0\n";
        while (*mnt && w < bufsize - 1) buf[w++] = *mnt++;
    } else if (path[0] == 's') {
        /* /proc/status — process table */
        const char *hdr = "PID  PRI  NICE  STATE  NAME\n";
        while (*hdr && w < bufsize - 1) buf[w++] = *hdr++;
        for (int i = 0; i < PROC_MAX && w < bufsize - 40; i++) {
            if (!proc_table[i].active) continue;
            /* Format: PID PRI NICE STATE NAME */
            char line[80];
            int li = 0;
            /* PID */
            char tmp[10]; int ti = 0;
            int v = proc_table[i].pid;
            if (v == 0) tmp[ti++] = '0';
            else while (v) { tmp[ti++] = '0' + v % 10; v /= 10; }
            while (ti--) line[li++] = tmp[ti];
            while (li < 5) line[li++] = ' ';
            /* PRI */
            ti = 0; v = proc_table[i].priority;
            if (v == 0) tmp[ti++] = '0';
            else while (v) { tmp[ti++] = '0' + v % 10; v /= 10; }
            while (ti--) line[li++] = tmp[ti];
            while (li < 10) line[li++] = ' ';
            /* NICE */
            int n = proc_table[i].nice;
            if (n < 0) { line[li++] = '-'; n = -n; }
            else line[li++] = ' ';
            ti = 0;
            if (n == 0) tmp[ti++] = '0';
            else while (n) { tmp[ti++] = '0' + n % 10; n /= 10; }
            while (ti--) line[li++] = tmp[ti];
            while (li < 16) line[li++] = ' ';
            /* STATE */
            const char *states[] = { "free", "run", "sleep", "zombie" };
            const char *st = states[proc_table[i].state & 3];
            while (*st) line[li++] = *st++;
            while (li < 23) line[li++] = ' ';
            /* NAME */
            const char *nm = proc_table[i].name;
            while (*nm && li < 78) line[li++] = *nm++;
            line[li++] = '\n';
            for (int j = 0; j < li && w < bufsize - 1; j++) buf[w++] = line[j];
        }
    } else if (path[0] == 'u') {
        /* /proc/uptime */
        uint64_t cnt, freq;
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
        if (freq == 0) freq = 62500000;
        unsigned long secs = (unsigned long)(cnt / freq);
        unsigned long frac = (unsigned long)((cnt % freq) * 100 / freq);
        char ubuf[32];
        int ui = 0;
        /* Format: secs.xx secs.xx */
        unsigned long s = secs;
        char tmp[20]; int ti = 0;
        if (s == 0) ubuf[ui++] = '0';
        else { while (s) { tmp[ti++] = '0' + s % 10; s /= 10; } while (ti--) ubuf[ui++] = tmp[ti]; }
        ubuf[ui++] = '.';
        ubuf[ui++] = '0' + (frac / 10);
        ubuf[ui++] = '0' + (frac % 10);
        ubuf[ui++] = ' ';
        /* idle time = same for now */
        s = secs; ti = 0;
        if (s == 0) ubuf[ui++] = '0';
        else { while (s) { tmp[ti++] = '0' + s % 10; s /= 10; } while (ti--) ubuf[ui++] = tmp[ti]; }
        ubuf[ui++] = '.';
        ubuf[ui++] = '0' + (frac / 10);
        ubuf[ui++] = '0' + (frac % 10);
        ubuf[ui++] = '\n';
        ubuf[ui] = 0;
        for (int i = 0; i < ui && w < bufsize - 1; i++) buf[w++] = ubuf[i];
    } else if (path[0] == 'l' && path[1] == 'o') {
        /* /proc/log — kernel log ring buffer */
        w = aios_log_read(buf, bufsize);
    } else {
        return -1;
    }
    if (w < bufsize) buf[w] = '\0';
    return w;
}

static int procfs_stat(void *ctx, const char *path, uint32_t *mode, uint32_t *size) {
    (void)ctx;
    *mode = 0100444; /* regular, read-only */
    *size = 0;
    return 0;
}

static int procfs_resolve(void *ctx, const char *path, uint32_t *ino) {
    (void)ctx;
    *ino = 2;
    return 0;
}

fs_ops_t procfs_ops = {
    .fs_list = procfs_list,
    .fs_read = procfs_read,
    .fs_stat = procfs_stat,
    .fs_resolve = procfs_resolve,
};

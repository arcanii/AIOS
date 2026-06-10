#include "aios/procfs.h"
#include "aios/version.h"
#include "aios/root_shared.h"
#include "aios/vka_audit.h"
#include "aios/hw_info.h"
#include "aios/aios_log.h"
#include "aios/blk_cache.h"
#include "aios/filehits.h"
#include "aios/cow.h"
#include <stdio.h>

#if defined(PLAT_RPI4)
/* v0.4.156: live GENET probe exposed at /proc/genet (impl in net_genet.c). */
int genet_diag_cmd(const char *args, char *buf, int bufsize);
#endif
/* Live xHCI/USB-keyboard probe + lock-LED poke at /proc/xhci (impl in src/usb/xhci.c).
 * Works on QEMU + RPi4, so unguarded. */
int xhci_diag_cmd(const char *args, char *buf, int bufsize);
/* System mouse state at /proc/mouse (impl in src/usb/xhci.c). */
int xhci_mouse_state(char *buf, int bufsize);
/* fb_console scroll/flush diagnostics at /proc/fbcon (impl in src/boot/fb_console.c). */
int fb_console_diag(char *buf, int bufsize);

proc_entry_t proc_table[PROC_MAX];
static int next_pid = 1;

void proc_init(void) {
    for (int i = 0; i < PROC_MAX; i++) proc_table[i].active = 0;
    /* PID 0: kernel/idle */
    proc_table[0].active = 1;
    proc_table[0].pid = 0;
    proc_table[0].priority = 255;
    proc_table[0].nice = -20;
    { const char *kn = "kernel"; int ki = 0; while (kn[ki]) { proc_table[0].name[ki] = kn[ki]; ki++; } proc_table[0].name[ki] = '\0'; }
    proc_table[0].state = 1;
}

int proc_add(const char *name, int priority) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (!proc_table[i].active) {
            proc_table[i].active = 1;
            proc_table[i].pid = next_pid++;
            proc_table[i].priority = priority;
            proc_table[i].nice = 0;
            int ni = 0;
            while (name[ni] && ni < 63) { proc_table[i].name[ni] = name[ni]; ni++; }
            proc_table[i].name[ni] = '\0';
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
    const char *entries[] = { "d .\n", "d ..\n", "- hw\n", "- version\n", "- uptime\n", "- mounts\n", "- status\n", "- log\n", "- meminfo\n", "- cpuinfo\n", "- stat\n", "- loadavg\n", "- vka\n", "- cachestats\n", "- filehits\n", "- serverstats\n", "- flush\n", "- cow\n", "- cmdline\n", "- xhci\n", "- mouse\n", "- fbcon\n",
#if defined(PLAT_RPI4)
        "- genet\n",
#endif
        "d self\n" };
    int n_entries = (int)(sizeof(entries) / sizeof(entries[0]));
    for (int i = 0; i < n_entries && w < bufsize - 1; i++) {
    
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
    if (path[0] == 'h' && path[1] == 'w') {
        /* /proc/hw -- hardware summary for login banner etc.
         * Line 1: cpu_compat cpu_count
         * Line 2: blk_type (emmc or virtio)
         * Line 3: net_type (genet, virtio, or none)
         * Line 4: ram_mb */
        {
            const char *cpu = hw_info.cpu_compat;
            while (*cpu && w < bufsize - 1) buf[w++] = *cpu++;
            buf[w++] = ' ';
            int nc = hw_info.cpu_count;
            if (nc >= 10) buf[w++] = '0' + nc / 10;
            buf[w++] = '0' + nc % 10;
            buf[w++] = '\n';
            const char *blk = hw_info.has_emmc ? "emmc" : "virtio";
            while (*blk && w < bufsize - 1) buf[w++] = *blk++;
            buf[w++] = '\n';
            const char *net = hw_info.has_genet ? "genet" :
                              hw_info.has_virtio ? "virtio" : "none";
            while (*net && w < bufsize - 1) buf[w++] = *net++;
            buf[w++] = '\n';
            /* RAM in MB */
            extern uint32_t aios_total_mem;
            uint32_t ram = aios_total_mem;
            char tmp[12]; int ti = 0;
            if (ram == 0) tmp[ti++] = '0';
            else while (ram) { tmp[ti++] = '0' + ram % 10; ram /= 10; }
            while (ti > 0) buf[w++] = tmp[--ti];
            buf[w++] = '\n';
        }
    } else if (path[0] == 'v' && path[1] == 'e') {
        /* /proc/version -- real version + build (version.h), dynamic CPU
         * info from DTB. The version/build/date come from the same macros
         * uname reports, so the two never drift. */
        {
            const char *p1 = AIOS_VERSION_FULL " (seL4 15.0.0, ";
            while (*p1 && w < bufsize - 1) buf[w++] = *p1++;
            const char *cp = hw_info.cpu_compat;
            while (*cp && w < bufsize - 1) buf[w++] = *cp++;
            const char *p2 = ", ";
            while (*p2 && w < bufsize - 1) buf[w++] = *p2++;
            int nc = hw_info.cpu_count;
            if (nc >= 10) buf[w++] = '0' + nc / 10;
            buf[w++] = '0' + nc % 10;
            const char *p3 = "-core SMP) " AIOS_BUILD_DATE "\n";
            while (*p3 && w < bufsize - 1) buf[w++] = *p3++;
        }
    } else if (path[0] == 'm' && path[1] == 'o' && path[2] == 'u' && path[3] == 's') {
        /* /proc/mouse -- USB mouse state (impl src/usb/xhci.c). Checked before
         * /proc/mounts since both start with "mo". */
        w = xhci_mouse_state(buf, bufsize);
    } else if (path[0] == 'm' && path[1] == 'o') {
        /* /proc/mounts -- v0.4.80: list actual VFS mounts */
        const char *lines[] = {
            "/dev/vda / ext2 rw 0 0\n",
            "proc /proc proc rw 0 0\n",
            "/dev/vdb /var/log ext2 rw 0 0\n",
            0
        };
        /* Check if log drive is mounted by testing vfs_read */
        int has_log = 0;
        {
            uint32_t _m, _s;
            extern int vfs_stat(const char *, uint32_t *, uint32_t *, uint32_t *);
            if (vfs_stat("/var/log", &_m, &_s, (void *)0) == 0) has_log = 1;
        }
        for (int mi = 0; lines[mi]; mi++) {
            if (mi == 2 && !has_log) continue;  /* skip /var/log if not mounted */
            const char *l = lines[mi];
            while (*l && w < bufsize - 1) buf[w++] = *l++;
        }
    } else if (path[0] == 's' && path[4] == 'u') {
        /* /proc/status — process table */
        const char *hdr = "PID  PRI  NICE  STATE  UID   THR  NAME\n";
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
            /* UID */
            ti = 0; v = (int)proc_table[i].uid;
            if (v == 0) tmp[ti++] = '0';
            else while (v) { tmp[ti++] = '0' + v % 10; v /= 10; }
            while (ti--) line[li++] = tmp[ti];
            while (li < 29) line[li++] = ' ';
            /* THR */
            ti = 0; v = proc_table[i].threads;
            if (v == 0) tmp[ti++] = '0';
            else while (v) { tmp[ti++] = '0' + v % 10; v /= 10; }
            while (ti--) line[li++] = tmp[ti];
            while (li < 34) line[li++] = ' ';
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
    } else if (path[0] == 'l' && path[1] == 'o' && path[2] == 'g') {
        /* /proc/log — kernel log ring buffer */
        w = aios_log_read(buf, bufsize);
    } else if (path[0] == 'm' && path[1] == 'e') {
        /* /proc/meminfo (v0.4.103: real numbers + VKA pool info)
         * aios_total_mem is in MB, so multiply by 1024 for kB. */
        extern int vka_live_frames;
        extern int vka_peak_frames;
        uint32_t total_kb = aios_total_mem * 1024;
        uint32_t pool_kb  = 8000 * 4;       /* 4 KB per page */
        uint32_t used_kb  = (uint32_t)(vka_live_frames < 0 ? 0 : vka_live_frames) * 4;
        uint32_t peak_kb  = (uint32_t)(vka_peak_frames < 0 ? 0 : vka_peak_frames) * 4;
        uint32_t free_kb  = pool_kb > used_kb ? pool_kb - used_kb : 0;

        /* Helper: append "Label: value unit\n" */
        #define APPEND_KV(label_str, val) do { \
            const char *L = (label_str); \
            while (*L && w < bufsize - 1) buf[w++] = *L++; \
            char _tmp[12]; int _ti = 0; uint32_t _v = (uint32_t)(val); \
            if (_v == 0) _tmp[_ti++] = '0'; \
            else { while (_v) { _tmp[_ti++] = '0' + _v % 10; _v /= 10; } } \
            while (_ti-- > 0 && w < bufsize - 1) buf[w++] = _tmp[_ti]; \
            const char *_u = " kB\n"; \
            while (*_u && w < bufsize - 1) buf[w++] = *_u++; \
        } while (0)

        APPEND_KV("MemTotal:       ", total_kb);
        APPEND_KV("PoolTotal:      ", pool_kb);
        APPEND_KV("PoolUsed:       ", used_kb);
        APPEND_KV("PoolFree:       ", free_kb);
        APPEND_KV("PoolPeak:       ", peak_kb);
        #undef APPEND_KV
    } else if (path[0] == 'c' && path[1] == 'p') {
        /* /proc/cpuinfo */
        for (int ci = 0; ci < hw_info.cpu_count && ci < 9; ci++) {
            const char *l1 = "processor\t: ";
            while (*l1 && w < bufsize - 1) buf[w++] = *l1++;
            buf[w++] = '0' + ci;
            buf[w++] = '\n';
            const char *l2 = "model name\t: ";
            while (*l2 && w < bufsize - 1) buf[w++] = *l2++;
            const char *cp = hw_info.cpu_compat;
            while (*cp && w < bufsize - 1) buf[w++] = *cp++;
            buf[w++] = '\n';
            buf[w++] = '\n';
        }
    } else if (path[0] == 'l' && path[1] == 'o' && path[2] == 'a') {
        /* /proc/loadavg */
        const char *la = "0.00 0.00 0.00 1/5 1\n";
        while (*la && w < bufsize - 1) buf[w++] = *la++;
    } else if (path[0] == 's' && path[4] == 0) {
        /* /proc/stat -- minimal kernel stats */
        const char *st = "cpu  0 0 0 0 0 0 0 0 0 0\n";
        while (*st && w < bufsize - 1) buf[w++] = *st++;
        for (int ci = 0; ci < hw_info.cpu_count && ci < 9; ci++) {
            const char *c = "cpu";
            while (*c && w < bufsize - 1) buf[w++] = *c++;
            buf[w++] = '0' + ci;
            const char *z = " 0 0 0 0 0 0 0 0 0 0\n";
            while (*z && w < bufsize - 1) buf[w++] = *z++;
        }
    } else if (path[0] >= '0' && path[0] <= '9') {
        /* /proc/[pid]/status */
        int pid = 0;
        const char *pp = path;
        while (*pp >= '0' && *pp <= '9') { pid = pid * 10 + (*pp - '0'); pp++; }
        /* Skip /status suffix if present */
        if (*pp == '/') pp++;
        /* Find process */
        for (int i = 0; i < PROC_MAX; i++) {
            if (proc_table[i].active && proc_table[i].pid == pid) {
                const char *states[] = { "free", "run", "sleep", "zombie" };
                /* Name: xxx */
                const char *lbl = "Name:\t";
                while (*lbl && w < bufsize - 1) buf[w++] = *lbl++;
                const char *nm = proc_table[i].name;
                while (*nm && w < bufsize - 1) buf[w++] = *nm++;
                buf[w++] = '\n';
                /* State: xxx */
                lbl = "State:\t";
                while (*lbl && w < bufsize - 1) buf[w++] = *lbl++;
                const char *st = states[proc_table[i].state & 3];
                while (*st && w < bufsize - 1) buf[w++] = *st++;
                buf[w++] = '\n';
                /* Pid: N */
                lbl = "Pid:\t";
                while (*lbl && w < bufsize - 1) buf[w++] = *lbl++;
                char tmp[12]; int ti = 0; int pv = pid;
                if (pv == 0) tmp[ti++] = '0';
                else { while (pv) { tmp[ti++] = '0' + pv % 10; pv /= 10; } }
                while (ti-- > 0 && w < bufsize - 1) buf[w++] = tmp[ti];
                buf[w++] = '\n';
                /* Uid: N */
                lbl = "Uid:\t";
                while (*lbl && w < bufsize - 1) buf[w++] = *lbl++;
                ti = 0; pv = (int)proc_table[i].uid;
                if (pv == 0) tmp[ti++] = '0';
                else { while (pv) { tmp[ti++] = '0' + pv % 10; pv /= 10; } }
                while (ti-- > 0 && w < bufsize - 1) buf[w++] = tmp[ti];
                buf[w++] = '\n';
                /* Priority: N */
                lbl = "Priority:\t";
                while (*lbl && w < bufsize - 1) buf[w++] = *lbl++;
                ti = 0; pv = proc_table[i].priority;
                if (pv == 0) tmp[ti++] = '0';
                else { while (pv) { tmp[ti++] = '0' + pv % 10; pv /= 10; } }
                while (ti-- > 0 && w < bufsize - 1) buf[w++] = tmp[ti];
                buf[w++] = '\n';
                /* Threads: N */
                lbl = "Threads:\t";
                while (*lbl && w < bufsize - 1) buf[w++] = *lbl++;
                ti = 0; pv = proc_table[i].threads;
                if (pv == 0) tmp[ti++] = '0';
                else { while (pv) { tmp[ti++] = '0' + pv % 10; pv /= 10; } }
                while (ti-- > 0 && w < bufsize - 1) buf[w++] = tmp[ti];
                buf[w++] = '\n';
                break;
            }
        }
    } else if (path[0] == 'v' && path[1] == 'k' && path[2] == 'a') {
        /* /proc/vka -- VKA allocator audit */
        extern int vka_live_frames;
        extern int vka_peak_frames;
        w += snprintf(buf + w, bufsize - w, "pool: 8000 pages\n");
        uint32_t total = 0;
        for (int i = 0; i < VKA_SUB_COUNT; i++) {
            vka_audit_entry_t *e = &vka_audit[i];
            if (e->total_pages)
                w += snprintf(buf + w, bufsize - w,
                    "%s: fr=%u cs=%u pg=%u\n",
                    vka_sub_names[i], e->frames, e->cslots, e->total_pages);
            total += e->total_pages;
        }
        w += snprintf(buf + w, bufsize - w,
            "alloc_total: %u\nlive: %d\npeak: %d\n",
            total, vka_live_frames, vka_peak_frames);
    } else if (path[0] == 'f' && path[1] == 'b') {
        /* /proc/fbcon -- fb_console scroll/flush diagnostics (HW scroll-freeze debug). */
        w = fb_console_diag(buf, bufsize);
    } else if (path[0] == 'f' && path[1] == 'i' && path[2] == 'l'
            && path[3] == 'e' && path[4] == 'h' && path[5] == 'i'
            && path[6] == 't' && path[7] == 's') {
        /* v0.4.114: /proc/filehits -- top accessed files */
        w = filehits_format(buf, bufsize, 30);
    } else if (path[0] == 's' && path[1] == 'e' && path[2] == 'r'
            && path[3] == 'v' && path[4] == 'e' && path[5] == 'r'
            && path[6] == 's' && path[7] == 't' && path[8] == 'a'
            && path[9] == 't' && path[10] == 's') {
        /* v0.4.121: /proc/serverstats -- in-process server health probe */
        w = serverstats_format(buf, bufsize);
    } else if (path[0] == 'f' && path[1] == 'l' && path[2] == 'u'
            && path[3] == 's' && path[4] == 'h' && path[5] == '\0') {
        /* v0.4.188: /proc/flush -- periodic write-back flusher stats */
        w = flush_server_format(buf, bufsize);
    } else if (path[0] == 'c' && path[1] == 'o' && path[2] == 'w'
            && path[3] == '\0') {
        /* v0.4.122: /proc/cow -- COW per-frame refcount stats (Phase 2 Step 2) */
        w = cow_format_stats(buf, bufsize);
    } else if (path[0] == 'c' && path[1] == 'm' && path[2] == 'd'
            && path[3] == 'l' && path[4] == 'i' && path[5] == 'n'
            && path[6] == 'e') {
        /* v0.4.129/131: /proc/cmdline -- one-line summary of the boot
         * environment. AIOS has no Linux-style boot args; this stands
         * in by stitching together the runtime-discovered values. */
        extern uint32_t aios_total_mem;
        const char *blk = hw_info.has_emmc ? "emmc" : "virtio";
        const char *net = hw_info.has_genet ? "genet" :
                          hw_info.has_virtio ? "virtio" : "none";
#if defined(PLAT_RPI4)
        const char *plat = "bcm2711";
#elif defined(PLAT_QEMU_VIRT)
        const char *plat = "qemu-virt";
#else
        const char *plat = "unknown";
#endif
        const char *root = hw_info.has_emmc ? "/dev/mmcblk0p2" : "/dev/vda";
        w += snprintf(buf + w, bufsize - w,
            "aios root=%s init=/bin/aios/getty platform=%s cpu=%s cores=%d ram=%uM blk=%s net=%s\n",
            root, plat, hw_info.cpu_compat, hw_info.cpu_count,
            (unsigned)aios_total_mem, blk, net);
    } else if (path[0] == 'c' && path[1] == 'a' && path[2] == 'c'
            && path[3] == 'h' && path[4] == 'e' && path[5] == 's'
            && path[6] == 't' && path[7] == 'a' && path[8] == 't'
            && path[9] == 's') {
        /* v0.4.112: /proc/cachestats -- block cache hit/miss/size */
        blk_cache_stats_t s;
        blk_cache_stats(&s);
        uint32_t lookups = s.hits + s.misses;
        uint32_t hit_pct = lookups ? (s.hits * 100u / lookups) : 0;
        w += snprintf(buf + w, bufsize - w,
            "hits: %u\nmisses: %u\nhit_rate_pct: %u\n"
            "pages: %u\npages_max: %u\n"
            "evicted: %u\nwrites: %u\nflushes: %u\ndirty: %u\n",
            s.hits, s.misses, hit_pct,
            s.pages, s.pages_max,
            s.evicted, s.writes, s.flushes, s.dirty);
        extern volatile uint32_t blk_poll_renotifies;
        w += snprintf(buf + w, bufsize - w,
            "blk_read_renotifies: %u\n", (unsigned)blk_poll_renotifies);
    } else if (path[0] == 'x' && path[1] == 'h' && path[2] == 'c'
            && path[3] == 'i') {
        /* /proc/xhci -- live USB xHCI + keyboard probe and lock-LED poke
         * (impl src/usb/xhci.c). cat /proc/xhci[.led.N|.lock]. Works on QEMU + RPi4. */
        int xw = xhci_diag_cmd(path + 4, buf, bufsize);
        if (xw < 0) return -1;
        w = xw;
#if defined(PLAT_RPI4)
    } else if (path[0] == 'g' && path[1] == 'e' && path[2] == 'n'
            && path[3] == 'e' && path[4] == 't') {
        /* v0.4.156: /proc/genet -- live GENET probe/poke (see net_genet.c).
         * cat /proc/genet[.cmd[.hexargs]] -- dump / peek / poke / mr / mw /
         * tx / reinit -- debug the RX datapath without reflashing. */
        int gw = genet_diag_cmd(path + 5, buf, bufsize);
        if (gw < 0) return -1;
        w = gw;
#endif
    } else {
        return -1;
    }
    if (w < bufsize) buf[w] = '\0';
    return w;
}

static int procfs_stat(void *ctx, const char *path, uint32_t *mode, uint32_t *size,
                       uint32_t *mtime) {
    (void)ctx;
    *size = 0;
    if (mtime) *mtime = 0;   /* /proc entries have no persistent mtime */
    /* /proc root */
    if (path[0] == '/' && path[1] == '\0') {
        *mode = 040555;
        return 0;
    }
    /* v0.4.79: /proc/self and /proc/self/fd directories */
    if (path[0] == '/' && path[1] == 's' && path[2] == 'e'
        && path[3] == 'l' && path[4] == 'f') {
        if (path[5] == 0) { *mode = 040555; return 0; }
        if (path[5] == '/') {
            if (path[6] == 'f' && path[7] == 'd') {
                if (path[8] == 0) { *mode = 040555; return 0; }
                if (path[8] == '/') { *mode = 0120777; return 0; }
            }
            if (path[6] == 'e' && path[7] == 'x' && path[8] == 'e'
                && path[9] == 0) { *mode = 0120777; return 0; }
        }
    }
    /* /proc/N -- pid directories */
    if (path[0] == '/' && path[1] >= '0' && path[1] <= '9') {
        const char *p = path + 1;
        while (*p >= '0' && *p <= '9') p++;
        if (*p == 0 || *p == '/') { *mode = 040555; return 0; }
    }
    *mode = 0100444;
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

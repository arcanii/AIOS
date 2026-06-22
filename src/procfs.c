#include "aios/procfs.h"
#include "aios/version.h"
#include "aios/root_shared.h"
#include "aios/vka_audit.h"
#include "aios/hw_info.h"
#include "aios/cpuacct.h"
#include "aios/aios_log.h"
#include "aios/blk_cache.h"
#include "aios/filehits.h"
#include "aios/cow.h"
#include "aios/net.h"
#include "aios/netd_stats.h"  /* /proc/net: the netd liveness page (s6) */
#ifdef AIOS_NETD
#include "aios/netd_ctrl.h"   /* NETD_DIAG_CRASH (the /proc/netd.crash demo trigger) */
#endif
#include <stdio.h>

#if defined(PLAT_RPI4)
/* v0.4.156: live GENET probe exposed at /proc/genet (impl in net_genet.c). */
int genet_diag_cmd(const char *args, char *buf, int bufsize);
#endif
/* Live xHCI/USB-keyboard probe + lock-LED poke at /proc/xhci (impl in src/usb/xhci.c).
 * Works on QEMU + RPi4, so unguarded. */
int xhci_diag_cmd(const char *args, char *buf, int bufsize);
/* Idle-core warmer A/B knob at /proc/corewarm (impl in src/servers/core_warm.c) -- a
 * stall-hunt diagnostic, inert until armed. */
int core_warm_cmd(const char *args, char *buf, int bufsize);
/* SCB-fabric keep-warm A/B knob at /proc/fabwarm (impl in src/servers/fabric_warm.c) --
 * the "Linux approach" to the idle-teardown DVM freeze; inert until armed. */
int fabric_warm_cmd(const char *args, char *buf, int bufsize);
/* Autonomous DRAM-DMA keep-warm A/B knob at /proc/dmawarm (impl in src/servers/dma_warm.c) --
 * session-8 cure attempt for the idle->wake cold-DRAM-load freeze; inert until armed. */
int dma_warm_cmd(const char *args, char *buf, int bufsize);
/* MVD-1 out-of-band watchdog status/knob at /proc/watchdog (impl in src/servers/watchdog.c)
 * -- core-0 liveness + a core-1 watchdog that survives the teardown freeze. */
int watchdog_cmd(const char *args, char *buf, int bufsize);
/* Serial-independent last-stall view at /proc/laststall (impl in src/servers/watchdog.c). */
int watchdog_laststall_cmd(char *buf, int bufsize);
/* ARM-local AXI_QUIET_TIME diagnostic at /proc/axiquiet (the fabric-quiesce detector). */
extern volatile uint32_t *dev_armlocal_vaddr;
/* System mouse state at /proc/mouse (impl in src/usb/xhci.c). */
int xhci_mouse_state(char *buf, int bufsize);
/* V3D GPU bring-up probe/poke at /proc/v3d (impl in src/gpu/v3d.c). Phase 0:
 * .power[.N] / .clock / register peek-poke. Prints "not present" on QEMU. */
int v3d_diag_cmd(const char *args, char *buf, int bufsize);
/* fb_console scroll/flush diagnostics at /proc/fbcon (impl in src/boot/fb_console.c). */
int fb_console_diag(char *buf, int bufsize);
/* Idle-teardown freeze counter at /proc/freezes (impl in src/servers/pipe_server.c).
 * The pipe server catches the ~33s BCM2711 fabric stall as an over-long msg handle. */
int pipe_freezes_format(char *buf, int bufsize);

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
    const char *entries[] = { "d .\n", "d ..\n", "- hw\n", "- version\n", "- uptime\n", "- mounts\n", "- status\n", "- log\n", "- meminfo\n", "- cpuinfo\n", "- stat\n", "- loadavg\n", "- vka\n", "- cachestats\n", "- netstat\n", "- filehits\n", "- serverstats\n", "- freezes\n", "- flush\n", "- cow\n", "- cmdline\n", "- xhci\n", "- mouse\n", "- fbcon\n", "- v3d\n", "- cpufreq\n", "- temp\n", "- cpuacct\n",
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
            const char *p3 = "-core SMP) " AIOS_BUILD_TIME "\n";
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
    } else if (path[0] == 't' && path[1] == 'e' && path[2] == 'm'
            && path[3] == 'p') {
        /* /proc/temp -- SoC temperature via the VC firmware mailbox (RPi4), in
         * degrees C. "unavailable" on QEMU (no VC mailbox / thermal sensor). */
        int cur = 0, max = 0;
        if (hw_soc_temp_mc(&cur, &max) == 0) {
            w += snprintf(buf + w, bufsize - w,
                "soc_temp_c: %d.%03d\nsoc_temp_max_c: %d.%03d\n",
                cur / 1000, cur % 1000, max / 1000, max % 1000);
        } else {
            w += snprintf(buf + w, bufsize - w,
                "soc temperature: unavailable (no VC mailbox)\n");
        }
    } else if (path[0] == 'c' && path[1] == 'p' && path[2] == 'u'
            && path[3] == 'f') {
        /* /proc/cpufreq -- live ARM core clock via the VC firmware mailbox (RPi4).
         * Read: clock (cur/max) + the DVFS load probe (loop_iters + a cntpct
         * timestamp; the rate across two reads tracks load). Write: .set.MHZ
         * manually sets the ARM clock (the DVFS power lever; the governor will use
         * the same hw_arm_clock_set call). "unavailable" on QEMU (no VC mailbox).
         * The static ceiling is arm_freq in config.txt (the heat cap); AIOS spins
         * all cores (no WFI -- the stall cure), so absent DVFS the firmware holds
         * the A72 at the cap. */
        const char *arg = path + 7;   /* text after "cpufreq" */
        if (arg[0] == '.' && arg[1] == 's' && arg[2] == 'e' && arg[3] == 't'
                && arg[4] == '.') {
            /* /proc/cpufreq.set.MHZ -- manual DVFS control (RPi4 HW experiment). */
            unsigned int mhz = 0;
            for (const char *p = arg + 5; *p >= '0' && *p <= '9'; p++)
                mhz = mhz * 10u + (unsigned int)(*p - '0');
            if (mhz < 100u) mhz = 100u;        /* sanity floor */
            if (mhz > 2000u) mhz = 2000u;       /* sanity ceiling (firmware clamps) */
            unsigned int got = 0;
            if (hw_arm_clock_set(mhz, &got) == 0)
                w += snprintf(buf + w, bufsize - w,
                    "arm clock set: req %u MHz -> got %u Hz (%u MHz)\n",
                    mhz, got, got / 1000000u);
            else
                w += snprintf(buf + w, bufsize - w,
                    "arm clock set: unavailable (no VC mailbox)\n");
        } else if (arg[0] == '.' && arg[1] == 'g' && arg[2] == 'o' && arg[3] == 'v'
                && arg[4] == '.') {
            /* /proc/cpufreq.gov.0 | .gov.1 -- toggle the load-driven DVFS governor.
             * When ON it owns the clock (a manual .set is overridden on the next
             * tick); turn it OFF to drive the clock manually with .set. */
            int on = (arg[5] == '1');
            cpu_gov_enable(on);
            w += snprintf(buf + w, bufsize - w, "governor %s\n",
                          on ? "enabled" : "disabled");
            w += cpu_gov_status(buf + w, bufsize - w);
        } else if (arg[0] == '.' && arg[1] == 't' && arg[2] == 'u' && arg[3] == 'n'
                && arg[4] == 'e' && arg[5] == '.') {
            /* /proc/cpufreq.tune.LO.HI.IT -- live governor thresholds (RPi4): LO/HI
             * are the busy permille (downclock < LO for IT ticks; upclock > HI) and
             * IT the idle-tick count. A zero field is left unchanged. Lets the
             * governor be dialled in over netconsole with no reflash per attempt. */
            unsigned vals[3] = {0, 0, 0};
            const char *p = arg + 6;
            for (int vi = 0; vi < 3; vi++) {
                while (*p >= '0' && *p <= '9') vals[vi] = vals[vi] * 10u + (unsigned)(*p++ - '0');
                if (*p == '.') p++;
            }
            cpu_gov_tune(vals[0], vals[1], (int)vals[2]);
            w += snprintf(buf + w, bufsize - w, "governor tuned\n");
            w += cpu_gov_status(buf + w, bufsize - w);
        } else {
            unsigned int cur = 0, max = 0;
            if (hw_arm_clock_hz(&cur, &max) == 0) {
                w += snprintf(buf + w, bufsize - w,
                    "arm_cur_hz: %u\narm_cur_mhz: %u\narm_max_hz: %u\narm_max_mhz: %u\n",
                    cur, cur / 1000000u, max, max / 1000000u);
            } else {
                w += snprintf(buf + w, bufsize - w,
                    "cpu clock: unavailable (no VC mailbox)\n");
            }
            /* DVFS load probe: raw root-loop counter + a cntpct timestamp. cntpct
             * (the generic timer) is fixed-rate / clock-independent, so two reads N
             * apart yield the loop rate -- which falls as work preempts core 0. */
            unsigned long long cnt = 0;
            __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
            w += snprintf(buf + w, bufsize - w,
                "loop_iters: %lu\ncntpct: %llu\n", g_root_loop_iters, cnt);
            w += cpu_gov_status(buf + w, bufsize - w);
        }
    } else if (path[0] == 'c' && path[1] == 'p' && path[2] == 'u'
            && path[3] == 'a') {
        /* /proc/cpuacct -- per-thread CPU-cycle accounting (seL4 utilisation).
         * Deltas since the last read, so read twice to see an interval. Finds
         * core-0 hogs + is the DVFS governor's real all-core idle signal. */
        w += cpuacct_render(buf + w, bufsize - w);
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
    } else if (path[0] == 'f' && path[1] == 'r' && path[2] == 'e'
            && path[3] == 'e' && path[4] == 'z') {
        /* v0.4.265: /proc/freezes -- idle-teardown whole-system stall counter
         * (the pipe server catches the ~33s fabric freeze as an over-long msg
         * handle). Lets the real in-use stall rate be measured over days. */
        w = pipe_freezes_format(buf, bufsize);
    } else if (path[0] == 'f' && path[1] == 'l' && path[2] == 'u'
            && path[3] == 's' && path[4] == 'h' && path[5] == '\0') {
        /* v0.4.188: /proc/flush -- periodic write-back flusher stats */
        w = flush_server_format(buf, bufsize);
    } else if (path[0] == 't' && path[1] == 'i' && path[2] == 'm'
            && path[3] == 'e' && path[4] == 'r' && path[5] == 's'
            && path[6] == 'l' && path[7] == 'e' && path[8] == 'e' && path[9] == 'p') {
        /* session-11: /proc/timersleep[.0|.1] -- gate whether newly-spawned procs get the
         * timer cap (Phase-2 userspace nanosleep BLOCK vs yield). Default 0; .1 to A/B. */
        int tw = timersleep_cmd(path + 10, buf, bufsize);
        if (tw < 0) return -1;
        w = tw;
    } else if (path[0] == 't' && path[1] == 'i' && path[2] == 'm'
            && path[3] == 'e' && path[4] == 'r' && path[5] == '\0') {
        /* session-11: /proc/timer -- system-timer blocking-sleep service stats (the HW
         * canary: wakes climbing + active bounded => the IRQ fires and replies). */
        w = timer_server_format(buf, bufsize);
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
            "evicted: %u\nwrites: %u\nflushes: %u\ndirty: %u\nread_multi: %u\nprefetch: %u\ndiscarded: %u\n",
            s.hits, s.misses, hit_pct,
            s.pages, s.pages_max,
            s.evicted, s.writes, s.flushes, s.dirty, s.read_multi, s.prefetch, s.discarded);
        extern volatile uint32_t blk_poll_renotifies;
        w += snprintf(buf + w, bufsize - w,
            "blk_read_renotifies: %u\n", (unsigned)blk_poll_renotifies);
        /* v0.4.224: eMMC data-phase completion timeouts (RPi4; 0 on QEMU).
         * retries = absorbed by the in-driver retry; fails = surfaced as
         * I/O errors after the retry also timed out. */
        extern volatile uint32_t blk_emmc_timeout_retries;
        extern volatile uint32_t blk_emmc_timeout_fails;
        w += snprintf(buf + w, bufsize - w,
            "emmc_timeout_retries: %u\nemmc_timeout_fails: %u\n",
            (unsigned)blk_emmc_timeout_retries,
            (unsigned)blk_emmc_timeout_fails);
    } else if (path[0] == 'n' && path[1] == 'e' && path[2] == 't'
            && path[3] == '\0') {
        /* /proc/net -- netd liveness + state, rendered IPC-FREE from the
         * cacheable stats page (DESIGN_NETD s6). Works even when netd is WEDGED
         * (unlike a Call), so it is the hung-netd detector + soak instrument.
         * NULL under flag-OFF (the in-root net stack uses /proc/netstat). */
        struct netd_stats *st = netd_stats_root;
        if (!st) {
            w += snprintf(buf + w, bufsize - w,
                "netd: not active (in-root net stack or no NIC)\n"
                "see /proc/netstat (counters) and /proc/serverstats (health)\n");
        } else {
            w += snprintf(buf + w, bufsize - w,
                "heartbeat: %llu\ndev_init_done: %u\n"
                "dhcp_bound: %u  lease_secs: %u  renews: %u\n"
                "mac: %02x:%02x:%02x:%02x:%02x:%02x\n"
                "ip: %u.%u.%u.%u  gw: %u.%u.%u.%u  mask: %u.%u.%u.%u\n"
                "last_err: %d  cleanup_lost: %u\n",
                (unsigned long long)st->heartbeat, st->dev_init_done,
                st->dhcp_bound, st->dhcp_lease_secs, st->dhcp_renews,
                st->mac[0], st->mac[1], st->mac[2], st->mac[3], st->mac[4], st->mac[5],
                st->ip[0], st->ip[1], st->ip[2], st->ip[3],
                st->gw[0], st->gw[1], st->gw[2], st->gw[3],
                st->mask[0], st->mask[1], st->mask[2], st->mask[3],
                st->last_err, st->cleanup_lost);
            for (int i = 0; i < NETD_STATS_SOCKS; i++)
                if (st->sock[i].in_use)
                    w += snprintf(buf + w, bufsize - w,
                        "sock%d: type=%u state=%u owner_pid=%d\n",
                        i, st->sock[i].type, st->sock[i].state, st->sock[i].owner_pid);
        }
#ifdef AIOS_NETD
    } else if (path[0] == 'n' && path[1] == 'e' && path[2] == 't'
            && path[3] == 'd') {
        /* /proc/netd -- netd control (DESIGN_NETD Stage 3). For now only the s10
         * crash-containment DEMO trigger: cat /proc/netd.crash fire-and-forgets a
         * NET_DIAG crash request to netd so the root fault listener can prove
         * containment + the reply-sweep. NBSend (never a Call): the fs thread must
         * never block on netd, and a non-blocking send drops harmlessly if netd is
         * already wedged/gone instead of wedging every /proc read. */
        if (path[4] == '.' && path[5] == 'c') {   /* .crash */
            if (net_ep_cap) {
                seL4_SetMR(0, (seL4_Word)NETD_DIAG_CRASH);
                seL4_NBSend(net_ep_cap, seL4_MessageInfo_new(NET_DIAG, 0, 0, 1));
                w += snprintf(buf + w, bufsize - w,
                    "netd crash trigger sent (NET_DIAG); watch the serial console "
                    "for the fault listener sweep\n");
            } else {
                w += snprintf(buf + w, bufsize - w,
                    "netd not active (net_ep_cap=0)\n");
            }
        } else {
            w += snprintf(buf + w, bufsize - w,
                "netd control: cat /proc/netd.crash faults netd (s10 demo)\n");
        }
    } else if (path[0] == 'n' && path[1] == 'e' && path[2] == 't'
            && path[3] == 's' && path[4] == 't' && path[5] == 'a'
            && path[6] == 't') {
#else
    } else if (path[0] == 'n' && path[1] == 'e' && path[2] == 't'
            && path[3] == 's' && path[4] == 't' && path[5] == 'a'
            && path[6] == 't') {
#endif
        /* v0.4.225: /proc/netstat -- inbound-path integrity counters for the
         * push-corruption hunt (docs/NEXT_20260612_net_rx_corruption.md).
         * Path verbs (xhci/v3d pattern):
         *   cat /proc/netstat          counters + current fault knob
         *   cat /proc/netstat.drop.N   drop every Nth inbound TCP data
         *                              segment (0 = off) -- deterministic
         *                              retransmit-storm injection for tests */
        if (path[7] == '.' && path[8] == 'r' && path[9] == 'e'
            && path[10] == 'n' && path[11] == 'e' && path[12] == 'w') {
            /* v0.4.233: force one immediate DHCP lease renewal (test hook). */
            net_dhcp_force_renew();
            w += snprintf(buf + w, bufsize - w,
                "dhcp renewal forced (watch dhcp_renews / dhcp_acks)\n");
        } else if (path[7] == '.' && path[8] == 'd' && path[9] == 'r'
            && path[10] == 'o' && path[11] == 'p' && path[12] == '.') {
            uint32_t n = 0;
            for (int i = 13; path[i] >= '0' && path[i] <= '9'; i++)
                n = n * 10 + (uint32_t)(path[i] - '0');
            net_fault_drop_nth = n;
            w += snprintf(buf + w, bufsize - w,
                "fault_drop_nth set to %u\n", (unsigned)n);
        } else if (path[7] == '.' && path[8] == 't' && path[9] == 'x'
            && path[10] == 'd' && path[11] == 'r' && path[12] == 'o'
            && path[13] == 'p' && path[14] == '.') {
            /* v0.4.253-fix: drop the next N OUTBOUND data/FIN segments (0=off). */
            uint32_t n = 0;
            for (int i = 15; path[i] >= '0' && path[i] <= '9'; i++)
                n = n * 10 + (uint32_t)(path[i] - '0');
            net_fault_tx_drop_n = n;
            w += snprintf(buf + w, bufsize - w,
                "fault_tx_drop_n set to %u\n", (unsigned)n);
        } else if (path[7] == '.' && path[8] == 'f' && path[9] == 'i'
            && path[10] == 'n' && path[11] == 'd' && path[12] == 'r'
            && path[13] == 'o' && path[14] == 'p' && path[15] == '.') {
            /* v0.4.253-fix: drop the next N OUTBOUND FIN segments (0=off). */
            uint32_t n = 0;
            for (int i = 16; path[i] >= '0' && path[i] <= '9'; i++)
                n = n * 10 + (uint32_t)(path[i] - '0');
            net_fault_fin_drop_n = n;
            w += snprintf(buf + w, bufsize - w,
                "fault_fin_drop_n set to %u\n", (unsigned)n);
        } else if (path[7] == '.' && path[8] == 'a' && path[9] == 'c'
            && path[10] == 'k' && path[11] == 'd' && path[12] == 'r'
            && path[13] == 'o' && path[14] == 'p' && path[15] == '.') {
            /* v0.4.253-fix: drop every Nth INBOUND pure-ACK (0=off). */
            uint32_t n = 0;
            for (int i = 16; path[i] >= '0' && path[i] <= '9'; i++)
                n = n * 10 + (uint32_t)(path[i] - '0');
            net_fault_ack_drop_nth = n;
            w += snprintf(buf + w, bufsize - w,
                "fault_ack_drop_nth set to %u\n", (unsigned)n);
        } else {
            w += snprintf(buf + w, bufsize - w,
                "tcp_data_segs: %u\ntcp_cksum_drops: %u\n"
                "tcp_dup_segs: %u\ntcp_overlap_trims: %u\n"
                "tcp_ooo_drops: %u\ntcp_window_partial: %u\n"
                "ring_overflow_drops: %u\nfault_drops: %u\n"
                "tcp_reader_handoff: %u\ntcp_split_deliver: %u\n"
                "dbg_ff_store_off: %u\ndbg_ff_read_off: %u\n"
                "dbg_store_bytes: %u\ndbg_read_bytes: %u\n"
                "tcp_read_acks: %u\nfault_drop_nth: %u\n",
                (unsigned)net_rx_stats.tcp_data_segs,
                (unsigned)net_rx_stats.tcp_cksum_drops,
                (unsigned)net_rx_stats.tcp_dup_segs,
                (unsigned)net_rx_stats.tcp_overlap_trims,
                (unsigned)net_rx_stats.tcp_ooo_drops,
                (unsigned)net_rx_stats.tcp_window_partial,
                (unsigned)net_rx_stats.ring_overflow_drops,
                (unsigned)net_rx_stats.fault_drops,
                (unsigned)net_rx_stats.tcp_reader_handoff,
                (unsigned)net_rx_stats.tcp_split_deliver,
                (unsigned)net_rx_stats.dbg_ff_store_off,
                (unsigned)net_rx_stats.dbg_ff_read_off,
                (unsigned)net_rx_stats.dbg_store_bytes,
                (unsigned)net_rx_stats.dbg_read_bytes,
                (unsigned)net_rx_stats.tcp_read_acks,
                (unsigned)net_fault_drop_nth);
            /* v0.4.253-fix: sender close/retransmit observability + loss knobs. */
            w += snprintf(buf + w, bufsize - w,
                "tcp_rtx_segs: %u\ntcp_rst_sent: %u\ntcp_giveups: %u\n"
                "tcp_tx_drops: %u\ntcp_ack_drops: %u\n"
                "fault_tx_drop_n: %u\nfault_fin_drop_n: %u\n"
                "fault_ack_drop_nth: %u\n",
                (unsigned)net_rx_stats.tcp_rtx_segs,
                (unsigned)net_rx_stats.tcp_rst_sent,
                (unsigned)net_rx_stats.tcp_giveups,
                (unsigned)net_rx_stats.tcp_tx_drops,
                (unsigned)net_rx_stats.tcp_ack_drops,
                (unsigned)net_fault_tx_drop_n,
                (unsigned)net_fault_fin_drop_n,
                (unsigned)net_fault_ack_drop_nth);
            /* v0.4.233: DHCP lease/renewal state (the renewal test reads these). */
            w += snprintf(buf + w, bufsize - w,
                "dhcp_acks: %u\ndhcp_renews: %u\ndhcp_lease_secs: %u\n",
                (unsigned)dhcp_acks, (unsigned)dhcp_renews,
                (unsigned)dhcp_lease_secs);
        }
    } else if (path[0] == 'x' && path[1] == 'h' && path[2] == 'c'
            && path[3] == 'i') {
        /* /proc/xhci -- live USB xHCI + keyboard probe and lock-LED poke
         * (impl src/usb/xhci.c). cat /proc/xhci[.led.N|.lock]. Works on QEMU + RPi4. */
        int xw = xhci_diag_cmd(path + 4, buf, bufsize);
        if (xw < 0) return -1;
        w = xw;
    } else if (path[0] == 'c' && path[1] == 'o' && path[2] == 'r' && path[3] == 'e'
            && path[4] == 'w' && path[5] == 'a' && path[6] == 'r' && path[7] == 'm') {
        /* /proc/corewarm[.0|.1] -- idle-core warmer A/B knob (stall hunt, src/servers/
         * core_warm.c). .1 arms cores 1/2/3 busy, .0 disarms, bare = status. */
        int cw = core_warm_cmd(path + 8, buf, bufsize);
        if (cw < 0) return -1;
        w = cw;
    } else if (path[0] == 'f' && path[1] == 'a' && path[2] == 'b'
            && path[3] == 'w' && path[4] == 'a' && path[5] == 'r' && path[6] == 'm') {
        /* /proc/fabwarm[.0|.1] -- SCB-fabric keep-warm A/B knob (the "Linux approach"
         * to the idle-teardown DVM freeze, src/servers/fabric_warm.c). .1 arms core-0
         * keep-warm, .0 disarms, bare = status. */
        int fw = fabric_warm_cmd(path + 7, buf, bufsize);
        if (fw < 0) return -1;
        w = fw;
    } else if (path[0] == 'd' && path[1] == 'm' && path[2] == 'a'
            && path[3] == 'w' && path[4] == 'a' && path[5] == 'r' && path[6] == 'm') {
        /* /proc/dmawarm[.0|.1] -- session-8 autonomous DRAM-DMA keep-warm (the cold-DRAM-load
         * cure attempt, src/servers/dma_warm.c). .1 arms a self-looping DMA that keeps the
         * SCB->memory path warm during idle, .0 disarms, bare = status. */
        int dw = dma_warm_cmd(path + 7, buf, bufsize);
        if (dw < 0) return -1;
        w = dw;
    } else if (path[0] == 'w' && path[1] == 'a' && path[2] == 't' && path[3] == 'c'
            && path[4] == 'h' && path[5] == 'd' && path[6] == 'o' && path[7] == 'g') {
        /* /proc/watchdog[.0|.1] -- MVD-1 out-of-band watchdog (src/servers/watchdog.c).
         * bare = status (core-0 hb age, stall count, worst/last ms); .0 disables (parks
         * the core-1 watchdog, frees core 1); .1 enables. */
        int wd = watchdog_cmd(path + 8, buf, bufsize);
        if (wd < 0) return -1;
        w = wd;
    } else if (path[0] == 'l' && path[1] == 'a' && path[2] == 's' && path[3] == 't'
            && path[4] == 's' && path[5] == 't' && path[6] == 'a' && path[7] == 'l'
            && path[8] == 'l') {
        /* /proc/laststall -- serial-independent last-detected-stall view (watchdog.c). */
        int ls = watchdog_laststall_cmd(buf, bufsize);
        if (ls < 0) return -1;
        w = ls;
    } else if (path[0] == 'a' && path[1] == 'x' && path[2] == 'i' && path[3] == 'q'
            && path[4] == 'u' && path[5] == 'i' && path[6] == 'e' && path[7] == 't') {
        /* /proc/axiquiet -- dump the ARM-local block (AXI_QUIET_TIME @ +0x30 is the BCM2711
         * fabric-quiesce detector). Diagnostic only; the cure space is closed. NULL if the
         * ARM-local page was not exposed as a device untyped. */
        volatile uint32_t *al = dev_armlocal_vaddr;
        if (!al) {
            w = snprintf(buf, bufsize, "axiquiet: ARM-local (0xFF800000) not mapped (not a device untyped)\n");
        } else {
            w = snprintf(buf, bufsize,
                "axiquiet: ARM-local 0xFF800000 mapped @ %p\n  +0x00=0x%08x +0x0C=0x%08x +0x30(AXI_QUIET_TIME)=0x%08x +0x34=0x%08x\n",
                (void *)al, al[0x00 / 4], al[0x0C / 4], al[0x30 / 4], al[0x34 / 4]);
        }
    } else if (path[0] == 'c' && path[1] == 'o' && path[2] == 'r' && path[3] == 'e'
            && path[4] == 's' && path[5] == 'c' && path[6] == 'h' && path[7] == 'e'
            && path[8] == 'd') {
        /* /proc/coresched[.0|.1] -- Stage S user-process core distribution kill switch
         * (src/boot/spawn_util.c). .0 pins new procs to core 0, .1 spreads (default). */
        int cs = coresched_cmd(path + 9, buf, bufsize);
        if (cs < 0) return -1;
        w = cs;
    } else if (path[0] == 's' && path[1] == 'h' && path[2] == 'm' && path[3] == 'r'
            && path[4] == 'i' && path[5] == 'n' && path[6] == 'g') {
        /* /proc/shmring[.0|.1] -- v0.4.258 direct SPSC SHM-ring kill switch
         * (src/servers/pipe_server.c). .1 arms the ring for pipes created afterwards,
         * .0 disarms (default). Decided per-pipe at create; HW-soak gated. */
        int sr = shmring_cmd(path + 7, buf, bufsize);
        if (sr < 0) return -1;
        w = sr;
    } else if (path[0] == 'v' && path[1] == '3' && path[2] == 'd') {
        /* /proc/v3d -- V3D GPU bring-up probe (impl src/gpu/v3d.c). path[1]=='3'
         * disambiguates from /proc/version and /proc/vka. cat /proc/v3d[.power[.N]
         * |.clock[.MHz]|.r.<off>|.c.<off>|.w.<off>.<val>]. Works on QEMU + RPi4. */
        int vw = v3d_diag_cmd(path + 3, buf, bufsize);
        if (vw < 0) return -1;
        w = vw;
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

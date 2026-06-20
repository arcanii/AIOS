/* watchdog.c -- v0.4.271 MVD-1: a core-1 out-of-band watchdog that SURVIVES the
 * ~32.4s idle-teardown freeze and reports it live, while core 0 is wedged.
 *
 * BACKGROUND (project_stall_hunt session-4): the freeze is core 0's teardown
 * `tlbi;dsb` hanging ~32s on the quiesced BCM2711 SCB fabric -- UNCURABLE from
 * software (every keep-warm/clock/register/voltage lever refuted). The residency
 * fix (v0.4.269) stopped it IPI-storming the idle siblings, but the siblings STILL
 * froze: the kernel idle thread runs with IRQs on, so during core 0's stall (core 0
 * holds the big kernel lock) each sibling's TIMER IRQ enters the kernel and blocks
 * in clh_lock_acquire ~32s. So "survive the stall" needs THREE things:
 *   (1) the residency fix      [done v0.4.269 -- no TLB-IPI to the watchdog core],
 *   (2) MASK the timer IRQ on the watchdog core (core 1) so it never enters the
 *       kernel for a tick -> never blocks on the BKL  [seL4 boot.c: skip
 *       setIRQState(IRQTimer) for core 1; IPIs stay enabled so it is still
 *       schedulable], and
 *   (3) the watchdog is PURE USERSPACE -- NO syscalls in the hot loop (any syscall
 *       takes the BKL and would block ~32s). Like fabric_warm.c's armed spin.
 *
 * MECHANISM: a low-prio core-0 heartbeat thread stamps g_core0_hb_tick = cntpct()
 * in core 0's idle gaps (it is frozen iff core 0 is wedged IN the kernel -- a
 * userspace thread cannot run then). The core-1 watchdog busy-loops reading that
 * stamp vs its own cntpct; when it goes stale > WD_STALL_MS it pokes the mini-UART
 * directly (dev_uart_vaddr) -- out-of-band, because the kernel printf / netconsole
 * are dead while core 0 is wedged. The win: "1-2 cores stuck beats whole-box
 * wedged" + live visibility of a freeze the FTDI serial often misses.
 *
 * /proc/watchdog: bare = status (enabled, stalls, worst/last ms); .0 = disable
 * (parks the watchdog, frees core 1 for a keep-warm A/B or coresched); .1 = enable.
 */
#include <stdint.h>
#include <stdio.h>
#include <sel4/sel4.h>
#include <sel4utils/thread.h>
#include <vspace/vspace.h>
#include "arch.h"                 /* arch_dsb */
#include "aios/root_shared.h"     /* vka, vspace, simple */
#include "aios/device_map.h"      /* dev_uart_vaddr (RPi4; NULL on QEMU) */
#define LOG_MODULE "watchdog"
#include "aios/aios_log.h"
#include "aios/cpuacct.h"

/* Core that hosts the watchdog. Its timer IRQ is masked in the kernel (seL4 boot.c),
 * so it must only ever run a PURE-USERSPACE busy-loop (no syscalls). Matches the
 * fabric_warm/core_warm "idle-only secondary" convention. */
#define WD_CORE        1
/* Staleness threshold. Normal teardowns complete in <1s; the freeze is ~32.4s. 9s
 * cleanly separates them (no normal op blocks core-0 userspace 9s) while detecting a
 * stall well before it ends. */
#define WD_STALL_MS    9000ull

/* Shared in the root vspace: core-0 liveness stamp (written by the core-0 heartbeat
 * thread, read by the core-1 watchdog). 64-bit aligned -> atomic read/write on A72. */
volatile uint64_t g_core0_hb_tick;
volatile int      g_wd_enabled = 0;     /* default-OFF: both threads park (no busy-loop). Enable
                                         * via /proc/watchdog.1. Off-by-default keeps the QEMU gate
                                         * + the shipped board's core-0/1 load unperturbed; the
                                         * kernel timer-mask on core 1 is harmless while parked. */
volatile uint64_t g_wd_stalls;          /* count of detected core-0 stalls */
volatile uint64_t g_wd_worst_ms;        /* worst detected stall duration */
volatile uint64_t g_wd_last_ms;         /* most recent detected stall duration */
volatile uint64_t g_wd_hb_iters;        /* core-0 heartbeat iteration counter (liveness of the probe itself) */
static seL4_CPtr  g_hb_ntfn;            /* parks the core-0 heartbeat when disabled (prio 200 -> wakes cleanly) */
static seL4_CPtr  g_wd_ntfn;            /* parks the core-1 watchdog when disabled (woken cross-core via IPI) */

static inline uint64_t wd_now(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}
static inline uint64_t wd_freq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v ? v : 54000000;
}

/* Out-of-band mini-UART (AUX) print. Bounded TX-empty poll (LSR bit5) so a full FIFO
 * cannot hang the watchdog; the kernel stops printing while core 0 is wedged, so the
 * FIFO drains and these writes go through. NULL on QEMU (no mini-UART mapping). */
static void wd_uart_puts(const char *s)
{
    volatile uint32_t *u = dev_uart_vaddr;
    if (!u) {
        return;
    }
    for (; *s; s++) {
        for (int i = 0; i < 200000 && !(u[0x54 / 4] & (1u << 5)); i++) {
            /* bounded wait for AUX_MU_LSR TX-empty (bit 5) */
        }
        u[0x40 / 4] = (uint32_t)(uint8_t)*s;   /* AUX_MU_IO */
    }
}
static void wd_uart_put_u(uint64_t v)
{
    char b[24];
    int n = 0;
    if (!v) {
        wd_uart_puts("0");
        return;
    }
    while (v && n < 23) {
        b[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    char o[25];
    int m = 0;
    while (n) {
        o[m++] = b[--n];
    }
    o[m] = 0;
    wd_uart_puts(o);
}

/* Core-0 liveness heartbeat: prio-1 busy-loop that stamps cntpct. It runs whenever
 * core 0 has no higher-prio (server) work -- effectively replacing the no-WFI idle
 * spin, so it costs nothing extra. The stamp is FROZEN exactly when core 0 is wedged
 * IN the kernel (teardown tlbi;dsb, IRQs off, BKL held) -- the signal the watchdog
 * watches. Pure userspace: no syscalls. */
static void wd_core0_hb_fn(void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;
    /* Core-0 liveness heartbeat. Runs at the SERVER priority (200) and yields after each
     * stamp, so it round-robins with the prio-200 servers that saturate core 0 (several
     * yield-spin -- e.g. serverstats' probe_sleep). A prio-1 thread NEVER gets CPU here
     * (the busy-polling servers starve it -- /proc/cpuacct: pipe/xhci/root/serverstats/flush
     * ~= 98%; this is also why the kernel idle never runs on core 0). At prio 200 + yield it
     * gets a slice every round-robin cycle -> g_core0_hb_tick stays fresh, and is FROZEN
     * exactly when core 0 is wedged IN the kernel (no userspace runs then). seL4_Yield is a
     * syscall, but this is core 0 (its timer is intact) -- only the core-1 watchdog must
     * avoid syscalls. Parks when disabled so the default-off path adds nothing to core 0. */
    for (;;) {
        while (!g_wd_enabled) {
            seL4_Wait(g_hb_ntfn, NULL);
        }
        while (g_wd_enabled) {
            g_core0_hb_tick = wd_now();
            g_wd_hb_iters++;
            seL4_Yield();
        }
    }
}

/* Core-1 watchdog: PURE-USERSPACE busy-loop (no syscalls -> survives the BKL stall
 * because core 1's timer IRQ is masked in the kernel). Edge-triggered: prints once
 * when core 0 goes stale and once when it recovers. Parks on the notification when
 * disabled (that seL4_Wait runs only while NOT stalled, so it is safe). */
static void wd_watchdog_fn(void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;
    uint64_t freq = wd_freq();
    uint64_t thresh = WD_STALL_MS * freq / 1000ull;
    uint64_t pace = freq / 20;          /* re-check ~every 50ms (pure cntpct spin) */
    int signaled = 0;
    uint64_t stall_start_hb = 0;
    for (;;) {
        if (!g_wd_enabled) {
            signaled = 0;
            seL4_Wait(g_wd_ntfn, NULL);  /* park (frees core 1); woken by /proc/watchdog.1 */
            continue;
        }
        uint64_t now = wd_now();
        uint64_t hb = g_core0_hb_tick;
        uint64_t age = (now > hb) ? (now - hb) : 0;
        if (age > thresh) {
            if (!signaled) {
                signaled = 1;
                stall_start_hb = hb;
                g_wd_stalls++;
                wd_uart_puts("\r\n[WDOG] core0 STALLED (core1 alive, no tick/BKL) -- stalls=");
                wd_uart_put_u(g_wd_stalls);
                wd_uart_puts("\r\n");
            }
        } else if (signaled) {
            signaled = 0;
            uint64_t dur_ms = (now - stall_start_hb) * 1000ull / freq;
            g_wd_last_ms = dur_ms;
            if (dur_ms > g_wd_worst_ms) {
                g_wd_worst_ms = dur_ms;
            }
            wd_uart_puts("[WDOG] core0 recovered after ");
            wd_uart_put_u(dur_ms);
            wd_uart_puts("ms\r\n");
        }
        uint64_t until = now + pace;
        while (wd_now() < until) {
            if (!g_wd_enabled) {
                break;       /* responsive disable */
            }
        }
    }
}

/* Spawn both threads at boot. Core-0 heartbeat always runs; the watchdog defaults
 * enabled. Boot thread: vka/vspace/cspace are single-owner, so allocate here. */
void watchdog_start(void)
{
    vka_object_t n1, n2;
    if (vka_alloc_notification(&vka, &n1) || vka_alloc_notification(&vka, &n2)) {
        printf("[watchdog] no ntfn -- watchdog unavailable\n");
        return;
    }
    g_hb_ntfn = n1.cptr;
    g_wd_ntfn = n2.cptr;
    g_core0_hb_tick = wd_now();

    /* core-0 liveness heartbeat (prio 200 == server prio, core 0): must match the busy
     * servers' priority to get scheduled in their round-robin (a lower prio starves). */
    sel4utils_thread_t hb;
    if (sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
            simple_get_cnode(&simple), seL4_NilData, &hb)) {
        printf("[watchdog] core0-hb configure failed\n");
        return;
    }
    seL4_TCB_SetPriority(hb.tcb.cptr, simple_get_tcb(&simple), 200);
#if CONFIG_MAX_NUM_NODES > 1
    seL4_TCB_SetAffinity(hb.tcb.cptr, 0);
#endif
    sel4utils_start_thread(&hb, wd_core0_hb_fn, NULL, NULL, 1);
    aios_acct_register("core0_hb", hb.tcb.cptr);

    /* core-1 out-of-band watchdog (prio 1, core WD_CORE -- timer-masked in kernel) */
    sel4utils_thread_t wd;
    if (sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
            simple_get_cnode(&simple), seL4_NilData, &wd)) {
        printf("[watchdog] core1-wd configure failed\n");
        return;
    }
    seL4_TCB_SetPriority(wd.tcb.cptr, simple_get_tcb(&simple), 1);
#if CONFIG_MAX_NUM_NODES > 1
    seL4_TCB_SetAffinity(wd.tcb.cptr, WD_CORE);
#endif
    sel4utils_start_thread(&wd, wd_watchdog_fn, NULL, NULL, 1);
    aios_acct_register("core1_wd", wd.tcb.cptr);

    printf("[watchdog] MVD-1 available (default-OFF): core-0 heartbeat + core-%d timer-masked watchdog; /proc/watchdog.1 to enable\n",
           WD_CORE);
}

/* /proc/watchdog[.0|.1] -- status + enable/disable. */
int watchdog_cmd(const char *args, char *buf, int bufsize)
{
    if (args[0] == '.' && args[1] == '1') {
        g_core0_hb_tick = wd_now();   /* fresh stamp so the watchdog does not false-trip at enable */
        g_wd_enabled = 1;
        arch_dsb();
        if (g_hb_ntfn) {
            seL4_Signal(g_hb_ntfn);   /* wake the parked core-0 heartbeat (prio 200 -> scheduled) */
        }
        if (g_wd_ntfn) {
            seL4_Signal(g_wd_ntfn);   /* wake the parked core-1 watchdog (cross-core reschedule IPI) */
        }
        return snprintf(buf, bufsize, "watchdog: ENABLED (core-%d out-of-band watchdog watching core 0)\n", WD_CORE);
    }
    if (args[0] == '.' && args[1] == '0') {
        g_wd_enabled = 0;
        arch_dsb();
        return snprintf(buf, bufsize, "watchdog: disabled (core-%d watchdog parked; core 1 free)\n", WD_CORE);
    }
    uint64_t now = wd_now();
    uint64_t age_ms = (now > g_core0_hb_tick) ? (now - g_core0_hb_tick) * 1000ull / wd_freq() : 0;
    return snprintf(buf, bufsize,
        "watchdog: enabled=%d core0_hb_age_ms=%llu hb_iters=%llu stalls=%llu worst_ms=%llu last_ms=%llu thresh_ms=%llu\n",
        g_wd_enabled, (unsigned long long)age_ms, (unsigned long long)g_wd_hb_iters,
        (unsigned long long)g_wd_stalls, (unsigned long long)g_wd_worst_ms,
        (unsigned long long)g_wd_last_ms, (unsigned long long)WD_STALL_MS);
}

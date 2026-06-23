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
#define WD_STALL_MS    9000ull        /* #3: detection threshold (tunable; 9s is proven false-trip-free) */

/* #1a: BCM2711 PM hardware watchdog (dev_pm_vaddr, paddr 0xFE100000) -- the same block
 * the `reboot` command uses (aios_root.c). Petted by the core-1 watchdog every loop so the
 * board AUTO-RECOVERS from a TOTAL wedge (one where even the timer-masked core-1 watchdog
 * dies, e.g. the manual-power-cycle case): no pet -> the SoC resets. A normal 32s core-0
 * freeze is NOT a total wedge (core 1 keeps petting), so it does NOT reset. Gated default-OFF
 * (/proc/watchdog.hwdog.1) -- arming it requires the watchdog enabled, and disabling either
 * disarms the PM block so the board never resets unexpectedly. */
#define WD_PM_PASSWORD  0x5A000000u
#define WD_PM_RSTC      (0x1C / 4)
#define WD_PM_WDOG      (0x24 / 4)
#define WD_PM_WDOG_MASK 0x000FFFFFu
#define WD_PM_RSTC_CLR  0xFFFFFFCFu
#define WD_PM_RSTC_FULL 0x00000020u
/* ~max timeout (20-bit field). PM watchdog ticks ~16 kHz -> ~64s; far longer than the ~50ms
 * pet interval (no spurious reset) yet a wedge auto-recovers within ~a minute. */
#define WD_PM_TIMEOUT   WD_PM_WDOG_MASK

/* #1b: ACT LED (GPIO 42, a direct SoC pin; aios_root.c sets GPFSEL4 to output). A visible
 * out-of-band stall signal -- no serial needed. dev_gpio_vaddr: GPSET1 @0x20, GPCLR1 @0x2c,
 * GPIO 42 -> bit (42-32)=10. */
#define WD_LED_GPSET1   (0x20 / 4)
#define WD_LED_GPCLR1   (0x2C / 4)
#define WD_LED_BIT      (1u << 10)

/* Shared in the root vspace: core-0 liveness stamp (written by the core-0 heartbeat
 * thread, read by the core-1 watchdog). 64-bit aligned -> atomic read/write on A72. */
volatile uint64_t g_core0_hb_tick;
volatile int      g_wd_enabled = 1;     /* v0.4.284 default-ON (unattended production): the
                                         * idle-teardown freeze is confirmed fabric-FUNDAMENTAL
                                         * (idle->wake, not curable in software -- session-7
                                         * localization), so the standing posture is SURVIVE +
                                         * report out-of-band + auto-recover. Both threads run from
                                         * boot; the core-1 watchdog is timer-masked + pure
                                         * userspace so it stays alive through a core-0 stall.
                                         * Disable via /proc/watchdog.0. (On QEMU this only adds a
                                         * busy core-1 vCPU -> a host-load timing shed in the gate,
                                         * not a real-board regression -- same artifact as fabwarm.) */
volatile uint64_t g_wd_stalls;          /* count of detected core-0 stalls */
volatile uint64_t g_wd_worst_ms;        /* worst detected stall duration */
volatile uint64_t g_wd_last_ms;         /* most recent detected stall duration */
volatile uint64_t g_wd_hb_iters;        /* core-0 heartbeat iteration counter (liveness of the probe itself) */
volatile uint64_t g_wd_last_tick;       /* #2: cntpct when the last stall was detected (/proc/laststall) */
volatile int      g_hwdog_armed;        /* #1a: PM HW-watchdog pet engaged (total-wedge auto-reset) */
static seL4_CPtr  g_hb_ntfn;            /* parks the core-0 heartbeat when disabled (prio 200 -> wakes cleanly) */
static seL4_CPtr  g_wd_ntfn;            /* parks the core-1 watchdog when disabled (woken cross-core via IPI) */

/* ── Confinement gate (Phase A step 3, the HW wedge-survival experiment) ─────────────────
 * The DECISIVE test for the symmetric-kernel premise: does work on a secondary SURVIVE a
 * peer core's wedge? Unlike the pure-userspace watchdog, this worker does a SYSCALL
 * (seL4_Yield -> NODE_LOCK) every iteration -- the same kernel entry any real work hits. If
 * a peer wedge HOLDS the BKL, the worker's syscall blocks ~32s and its tick FREEZES; if the
 * wedge is BKL-FREE, the tick keeps advancing. The core-1 watchdog (which survives) samples
 * the delta across a [WDOG] stall and reports it out-of-band. Default DISARMED;
 * /proc/confine.N arms it on core N (a secondary != WD_CORE and != the wedge core). */
volatile int      g_confine_armed;       /* worker loops while set */
volatile int      g_confine_core = -1;   /* core the worker is pinned to (-1 = unset) */
volatile uint64_t g_confine_ticks;       /* worker iterations = syscalls completed (the observable) */
static seL4_CPtr  g_confine_ntfn;        /* parks the worker when disarmed */
static sel4utils_thread_t g_confine_thread;
static int        g_confine_started;

/* #1b: drive the ACT LED (no syscall; harmless if GPIO unmapped / not output). */
static inline void wd_led(int on)
{
    volatile uint32_t *g = dev_gpio_vaddr;
    if (g) {
        g[on ? WD_LED_GPSET1 : WD_LED_GPCLR1] = WD_LED_BIT;
    }
}
/* #1a: refresh the PM hardware watchdog (no syscall -- a plain MMIO write). */
static inline void wd_pm_pet(void)
{
    volatile uint32_t *pm = dev_pm_vaddr;
    if (pm) {
        pm[WD_PM_WDOG] = WD_PM_PASSWORD | (WD_PM_TIMEOUT & WD_PM_WDOG_MASK);
    }
}
/* #1a: arm (start the countdown + enable reset-on-expiry) / disarm the PM watchdog. */
static void wd_pm_arm(void)
{
    volatile uint32_t *pm = dev_pm_vaddr;
    if (!pm) {
        return;
    }
    pm[WD_PM_WDOG] = WD_PM_PASSWORD | (WD_PM_TIMEOUT & WD_PM_WDOG_MASK);
    uint32_t rstc = pm[WD_PM_RSTC];
    pm[WD_PM_RSTC] = WD_PM_PASSWORD | (rstc & WD_PM_RSTC_CLR) | WD_PM_RSTC_FULL;
}
static void wd_pm_disarm(void)
{
    volatile uint32_t *pm = dev_pm_vaddr;
    if (!pm) {
        return;
    }
    uint32_t rstc = pm[WD_PM_RSTC];
    pm[WD_PM_RSTC] = WD_PM_PASSWORD | (rstc & WD_PM_RSTC_CLR);   /* clear FULL_RESET -> no reset on expiry */
}

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
    uint64_t confine_at_start = 0;        /* g_confine_ticks captured at stall onset */
    uint64_t confine_next_report = 0;     /* cntpct of the next mid-stall worker sample */
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
                confine_at_start = g_confine_ticks;        /* gate: snapshot the worker */
                confine_next_report = now + 2 * freq;      /* first mid-stall sample in ~2s */
                g_wd_last_tick = now;     /* #2: record for /proc/laststall */
                g_wd_stalls++;
                wd_led(1);                /* #1b: ACT LED ON for the duration of the stall */
                wd_uart_puts("\r\n[WDOG] core0 STALLED (core1 alive, no tick/BKL) -- stalls=");
                wd_uart_put_u(g_wd_stalls);
                if (g_confine_armed) {
                    wd_uart_puts(" confine_core=");
                    wd_uart_put_u((uint64_t)g_confine_core);
                    wd_uart_puts(" worker_ticks=");
                    wd_uart_put_u(confine_at_start);
                }
                wd_uart_puts("\r\n");
            } else if (g_confine_armed && now >= confine_next_report) {
                /* GATE: live worker sample DURING the stall. If this delta keeps growing,
                 * the syscall-doing worker on the secondary is SURVIVING the wedge (kernel
                 * entry is not BKL-blocked); if it stays flat, it froze. */
                confine_next_report = now + 2 * freq;
                wd_uart_puts("[WDOG] ...stalling, worker advanced=");
                wd_uart_put_u(g_confine_ticks - confine_at_start);
                wd_uart_puts("\r\n");
            }
        } else if (signaled) {
            signaled = 0;
            uint64_t dur_ms = (now - stall_start_hb) * 1000ull / freq;
            g_wd_last_ms = dur_ms;
            if (dur_ms > g_wd_worst_ms) {
                g_wd_worst_ms = dur_ms;
            }
            wd_led(0);                    /* #1b: ACT LED OFF on recovery */
            wd_uart_puts("[WDOG] core0 recovered after ");
            wd_uart_put_u(dur_ms);
            if (g_confine_armed) {
                /* THE DECISIVE METRIC: worker syscalls completed DURING the core-0 wedge.
                 * >0 => work on the secondary SURVIVED (BKL-free wedge -> Phase B viable);
                 * 0  => it froze on a kernel entry (BKL-held wedge -> premise broken). */
                wd_uart_puts("ms; confine worker(core ");
                wd_uart_put_u((uint64_t)g_confine_core);
                wd_uart_puts(") advanced=");
                wd_uart_put_u(g_confine_ticks - confine_at_start);
                wd_uart_puts(" during the wedge\r\n");
            } else {
                wd_uart_puts("ms\r\n");
            }
        }
        /* #1a: refresh the PM HW watchdog while armed (no syscall). Done every loop so a
         * normal core-0 freeze (core 1 still looping) never resets, but a TOTAL wedge
         * (core 1 dead -> this loop stops) lets the PM watchdog expire -> SoC reset. */
        if (g_hwdog_armed) {
            wd_pm_pet();
        }
        uint64_t until = now + pace;
        while (wd_now() < until) {
            if (!g_wd_enabled) {
                break;       /* responsive disable */
            }
        }
    }
}

/* Confinement-gate worker: pinned to g_confine_core, does a SYSCALL (seL4_Yield) per
 * iteration and bumps g_confine_ticks. The syscall is the kernel entry under test -- it
 * blocks ~32s iff a peer wedge holds the BKL. A short userspace spin moderates the BKL
 * hammering so the worker does not maximally starve core 0's servers. Parks when disarmed. */
static void wd_confine_worker_fn(void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;
    for (;;) {
        while (!g_confine_armed) {
            seL4_Wait(g_confine_ntfn, NULL);
        }
        while (g_confine_armed) {
            seL4_Yield();                 /* SYSCALL: NODE_LOCK_SYS -- the entry under test */
            g_confine_ticks++;
            for (volatile int i = 0; i < 4000; i++) { /* moderate the rate */ }
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
    /* SURVIVE infra: stays pinned to core 0 (the core-0 stall detector) -- NOT routed
     * through aios_server_pin / /proc/distribute. See Phase A step 2 note. */
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
    /* SURVIVE infra: stays pinned to WD_CORE (kernel-timer-masked, so it keeps running
     * through a core-0 wedge) -- NOT routed through aios_server_pin / /proc/distribute. */
    seL4_TCB_SetAffinity(wd.tcb.cptr, WD_CORE);
#endif
    sel4utils_start_thread(&wd, wd_watchdog_fn, NULL, NULL, 1);
    aios_acct_register("core1_wd", wd.tcb.cptr);

    /* Confinement-gate worker (Phase A step 3): created DISARMED -- armed onto a chosen
     * secondary core via /proc/confine.N for the HW wedge-survival experiment. Prio 200 so
     * it actually runs on its core; its default affinity is irrelevant while parked. */
#if CONFIG_MAX_NUM_NODES > 1
    {
        vka_object_t n3;
        if (!vka_alloc_notification(&vka, &n3)) {
            g_confine_ntfn = n3.cptr;
            if (!sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
                    simple_get_cnode(&simple), seL4_NilData, &g_confine_thread)) {
                seL4_TCB_SetPriority(g_confine_thread.tcb.cptr, simple_get_tcb(&simple), 200);
                if (!sel4utils_start_thread(&g_confine_thread, wd_confine_worker_fn, NULL, NULL, 1)) {
                    aios_acct_register("confine_wkr", g_confine_thread.tcb.cptr);
                    g_confine_started = 1;
                }
            }
        }
    }
#endif

    /* v0.4.284: default-ON PM HW-watchdog auto-recovery on real HW (the PM block is mapped only
     * on the Pi; dev_pm_vaddr is NULL on QEMU -> stays disarmed there). A normal ~32s freeze keeps
     * core 1 petting (no reset); a TOTAL cluster wedge stops the petting -> the SoC auto-resets in
     * ~63s. The box no longer goes silently dark unattended. Disable via /proc/watchdog.hwdog.0. */
    if (dev_pm_vaddr) {
        wd_pm_arm();
        g_hwdog_armed = 1;
        arch_dsb();
    }

    printf("[watchdog] MVD-1 DEFAULT-ON (unattended production): core-0 heartbeat + core-%d "
           "timer-masked watchdog%s; /proc/watchdog.0 to disable\n",
           WD_CORE, g_hwdog_armed ? " + PM hwdog auto-reset ARMED" : " (hwdog n/a -- no PM block)");
}

/* #2: /proc/laststall -- serial-independent view of the most recent detected stall (the
 * FTDI serial adapter is flaky; this is readable over netconsole). */
int watchdog_laststall_cmd(char *buf, int bufsize)
{
    if (!g_wd_stalls) {
        return snprintf(buf, bufsize, "laststall: none detected (watchdog %s)\n",
                        g_wd_enabled ? "enabled" : "DISABLED -- /proc/watchdog.1 to enable");
    }
    uint64_t now = wd_now();
    uint64_t ago_ms = (now > g_wd_last_tick) ? (now - g_wd_last_tick) * 1000ull / wd_freq() : 0;
    return snprintf(buf, bufsize,
        "laststall: %llums ago, last_dur=%llums worst_dur=%llums total=%llu (watchdog %s)\n",
        (unsigned long long)ago_ms, (unsigned long long)g_wd_last_ms,
        (unsigned long long)g_wd_worst_ms, (unsigned long long)g_wd_stalls,
        g_wd_enabled ? "enabled" : "disabled");
}

/* /proc/confine[.N|.r] -- Phase A step 3 wedge-survival gate. .N arms the syscall-doing worker
 * pinned to core N (the survivor under test; pick a secondary != WD_CORE and != the wedge core);
 * .r disarms. Then force a teardown-after-idle wedge and watch sercap: the [WDOG] line reports
 * "worker(core N) advanced=K during the wedge" -- K>0 means work on core N SURVIVED the wedge
 * (kernel entry was NOT BKL-blocked -> the symmetric-kernel premise holds, Phase B viable);
 * K==0 means it froze on a syscall (BKL-held wedge -> premise broken). */
int confine_cmd(const char *args, char *buf, int bufsize)
{
#if CONFIG_MAX_NUM_NODES > 1
    if (!g_confine_started) {
        return snprintf(buf, bufsize, "confine: worker unavailable (thread start failed)\n");
    }
    if (args[0] == '.') {
        if (args[1] == 'r') {
            g_confine_armed = 0;
            arch_dsb();
        } else if (args[1] >= '0' && args[1] <= '9') {
            int core = args[1] - '0';
            if (core < CONFIG_MAX_NUM_NODES) {
                g_confine_core = core;
                seL4_TCB_SetAffinity(g_confine_thread.tcb.cptr, (seL4_Word)core);
                g_confine_ticks = 0;
                g_confine_armed = 1;
                arch_dsb();
                if (g_confine_ntfn) {
                    seL4_Signal(g_confine_ntfn);   /* unpark the worker */
                }
            }
        }
    }
    return snprintf(buf, bufsize,
        "confine: armed=%d core=%d ticks=%llu (.N arm syscall-worker on core N / .r disarm); "
        "watch the [WDOG] \"worker advanced\" delta across a stall\n",
        g_confine_armed, g_confine_core, (unsigned long long)g_confine_ticks);
#else
    (void)args;
    return snprintf(buf, bufsize, "confine: single-core build (no-op)\n");
#endif
}

/* /proc/watchdog[.0|.1] -- status + enable/disable; .hwdog.[0|1] -- PM HW-watchdog auto-reset. */
int watchdog_cmd(const char *args, char *buf, int bufsize)
{
    /* #1a: .hwdog.1 / .hwdog.0 -- arm/disarm the PM hardware watchdog (total-wedge auto-reset). */
    if (args[0] == '.' && args[1] == 'h' && args[2] == 'w' && args[3] == 'd'
            && args[4] == 'o' && args[5] == 'g') {
        if (args[6] == '.' && args[7] == '1') {
            if (!g_wd_enabled) {
                return snprintf(buf, bufsize, "hwdog: refused -- enable the watchdog first (/proc/watchdog.1)\n");
            }
            if (!dev_pm_vaddr) {
                return snprintf(buf, bufsize, "hwdog: unavailable -- PM block not mapped (QEMU?)\n");
            }
            wd_pm_arm();
            g_hwdog_armed = 1;
            arch_dsb();
            return snprintf(buf, bufsize, "hwdog: ARMED -- PM watchdog petted by core %d; a TOTAL wedge auto-resets the SoC (~%us)\n",
                            WD_CORE, (unsigned)(WD_PM_TIMEOUT / 16384u));
        }
        if (args[6] == '.' && args[7] == '0') {
            g_hwdog_armed = 0;   /* stop petting FIRST */
            arch_dsb();
            wd_pm_disarm();      /* then clear reset-on-expiry -> board will not reset */
            return snprintf(buf, bufsize, "hwdog: disarmed (no auto-reset)\n");
        }
        return snprintf(buf, bufsize, "hwdog: armed=%d (.1 arm / .0 disarm; requires watchdog enabled)\n", g_hwdog_armed);
    }
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
        /* Safety: stop petting + disarm the PM watchdog BEFORE parking the loop, so disabling
         * the watchdog can never leave the SoC counting down to a reset. Also clear the LED. */
        if (g_hwdog_armed) {
            g_hwdog_armed = 0;
            arch_dsb();
            wd_pm_disarm();
        }
        wd_led(0);
        g_wd_enabled = 0;
        arch_dsb();
        return snprintf(buf, bufsize, "watchdog: disabled (core-%d watchdog parked; core 1 free; hwdog off)\n", WD_CORE);
    }
    uint64_t now = wd_now();
    uint64_t age_ms = (now > g_core0_hb_tick) ? (now - g_core0_hb_tick) * 1000ull / wd_freq() : 0;
    return snprintf(buf, bufsize,
        "watchdog: enabled=%d hwdog=%d core0_hb_age_ms=%llu hb_iters=%llu stalls=%llu worst_ms=%llu last_ms=%llu thresh_ms=%llu\n",
        g_wd_enabled, g_hwdog_armed, (unsigned long long)age_ms, (unsigned long long)g_wd_hb_iters,
        (unsigned long long)g_wd_stalls, (unsigned long long)g_wd_worst_ms,
        (unsigned long long)g_wd_last_ms, (unsigned long long)WD_STALL_MS);
}

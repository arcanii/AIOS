/* fabric_warm.c -- v0.4.266: BCM2711 SCB-fabric keep-warm (the "Linux approach"
 * to the ~33s idle-teardown TLBI/DVM whole-system freeze).
 *
 * MECHANISM (candidate 2 -- docs/NEXT_20260619_candidate2_fabric_dvm.md,
 * [[project_stall_hunt]]): the teardown `tlbi ; dsb` emits a DVM Sync that the
 * `dsb` cannot retire until the BCM2711 SCB / 128-bit AMBA fabric returns
 * DVM-Complete (A72 TRM L2ACTLR[11] "generated regardless"; BRESP gated, TRM
 * 7.7.4). After idle the SoC asserts ACINACTM to idle the AXI master snoop
 * interface (TRM 2.4) / clock-starves that fabric, so the FIRST post-idle DVM
 * Sync hangs to a ~33s SoC timeout. Linux never freezes because its constant
 * timer/DMA bus traffic keeps the fabric warm. AIOS's no-WFI busy-`yield` idle
 * keeps the CPU executing but makes NO bus transactions, so the fabric still
 * quiesces.
 *
 * This thread replicates Linux's immunity. When armed it busy-loops on CORE 1
 * (which is idle-only under the all-threads-pinned-to-core-0 model, so
 * monopolizing it starves nothing -- exactly like the proven `core_warm`),
 * issuing a steady stream of LIGHT UNCACHED reads of a VideoCore-side register
 * (GPLEV0). Each read traverses the ARM->VideoCore AXI link and keeps the
 * cluster's snoop/DVM path (shared by all 4 cores) out of the quiesced state, so
 * core 0's teardown DVM-Sync completes. It must NOT run on core 0: a busy-loop
 * there monopolizes the shell/netconsole core and hangs a blocked shell (HW-
 * confirmed -- the first cut on core 0 wedged the board until disarmed). And it
 * is NOT the refuted heavy-cacheable `corewarm` (cores 1-3 256KB RMW, which
 * WORSENED the freeze via DVM contention): GPLEV0 is Device memory, so the read
 * generates fabric traffic with NO cacheable coherency/DVM transactions -- bus
 * warmth without snoop contention.
 *
 * /proc/fabwarm.1 = arm (keep-warm ON), .0 = disarm (parked -> normal idle),
 * bare = status + iteration counter. Default DISARMED (inert -- zero effect on
 * the shipped system) so it can be A/B-tested against the baseline on the SAME
 * boot via /proc/freezes + pingmon.
 */
#include <stdio.h>
#include <sel4/sel4.h>
#include <sel4utils/thread.h>
#include <vspace/vspace.h>
#include "arch.h"                 /* arch_dsb */
#include "aios/root_shared.h"     /* vka, vspace, simple */
#include "aios/device_map.h"      /* dev_gpio_vaddr (RPi4; NULL on QEMU) */
#define LOG_MODULE "fabwarm"
#include "aios/aios_log.h"
#include "aios/cpuacct.h"

/* GPIO input-level register (0x34): read-only, no side effects, VideoCore-side
 * (0xFE200000) so the read crosses the ARM->VC AXI fabric. */
#define GPLEV0_IDX  (0x34 / 4)
/* Fabric-touch rate. One uncached read per ~1ms keeps the SCB AXI link out of
 * the quiesced state (the research minimum is "every tens of ms") while adding
 * negligible fabric load -- far below saturating it against GENET DMA. */
#define FABWARM_HZ  1000u

/* v0.4.269 GENET-MDIO mode (session-4): the session-3 primary-source research found
 * Linux's RPi4 immunity is its ~1 Hz GENET PHY link-poll over MDIO (BCM2711 GENET has no
 * link IRQ -> phylib PHY_POLL=HZ), a DIFFERENT SCB sub-block than the refuted GPLEV0/GPIO
 * read. Mode 2 mirrors it: a periodic MDIO read of the PHY status register via the root's
 * GENET mapping (dev_genet_vaddr). Each MDIO read kicks the UMAC MDIO state machine -> a
 * burst of MDIO-clock activity crossing the GENET block on the SCB. netd does NOT auto-poll
 * MDIO (only at init + on-demand /proc/genet), so a core-1 keep-warm does not contend with
 * it -- DO NOT run /proc/genet during a keep-warm soak. Register layout mirrors net_genet.c. */
#define UMAC_MDIO_CMD_IDX   ((0x0800 + 0x614) / 4)
#define MDIO_START_BUSY     (1u << 29)
#define MDIO_READ           (2u << 26)
#define MDIO_PMD_SHIFT      21
#define MDIO_REG_SHIFT      16
#define FABWARM_PHY_ADDR    1   /* BCM54213 default PHY address (net_genet.c) */
#define FABWARM_MII_BMSR    1   /* PHY Basic Mode Status register */

/* Keep-warm mode: 1 = GPLEV0/GPIO read (refuted), 2 = GENET MDIO read (Linux's lever). */
static volatile int      g_fabwarm_mode = 1;
static volatile int      g_fabwarm_armed;
static seL4_CPtr         g_fabwarm_ntfn;
static volatile uint64_t g_fabwarm_iters;

/* One uncached MDIO read of the PHY status register via the GENET block. Kicks the MDIO
 * state machine then polls START_BUSY clear (bounded -- never hang the core-1 spin). The
 * result is discarded; this is a fabric-traffic generator, not a link check. */
static inline void genet_mdio_touch(void)
{
    volatile uint32_t *g = dev_genet_vaddr;
    if (!g) {
        return;   /* QEMU / GENET unmapped */
    }
    g[UMAC_MDIO_CMD_IDX] = MDIO_START_BUSY | MDIO_READ |
                           ((uint32_t)FABWARM_PHY_ADDR << MDIO_PMD_SHIFT) |
                           ((uint32_t)FABWARM_MII_BMSR << MDIO_REG_SHIFT);
    arch_dsb();
    for (int i = 0; i < 4000; i++) {
        if (!(g[UMAC_MDIO_CMD_IDX] & MDIO_START_BUSY)) {
            break;
        }
    }
    (void)g[UMAC_MDIO_CMD_IDX];   /* read the result word (more GENET-block traffic) */
}

/* One uncached read of a VideoCore-side register = one ARM->VC AXI transaction.
 * On QEMU dev_gpio_vaddr is NULL (no-op; the fabric freeze does not exist there). */
static inline void fabric_touch(void)
{
    if (g_fabwarm_mode == 2) {
        genet_mdio_touch();
        return;
    }
    volatile uint32_t *g = dev_gpio_vaddr;
    if (g) {
        (void)g[GPLEV0_IDX];
    }
}

/* Generic-timer counter -- a cheap userspace register read (no syscall, no
 * fabric crossing), used to pace the fabric touches. */
static inline uint64_t fabwarm_now(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

/* Pinned to core 1 (idle-only): busy-warms the shared SCB fabric. Parks when
 * disarmed.
 *
 * CRITICAL: this is a PURE USERSPACE spin -- it must NOT call seL4_Yield (or any
 * syscall) in the hot loop. A yield-per-iteration hammers the SMP big-kernel-lock
 * and degrades the whole system. core_warm proves a syscall-free busy-loop on
 * cores 1-3 is safe (the corewarm A/Bs drove fine over netconsole). The only
 * non-idle work on core 1 is this thread, so monopolizing core 1 is harmless --
 * unlike core 0, where a busy-loop hung the blocked shell (HW-confirmed). */
static void fabric_warm_fn(void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (!freq) {
        freq = 54000000;
    }
    uint64_t interval = freq / FABWARM_HZ;   /* counter ticks between touches */
    for (;;) {
        seL4_Wait(g_fabwarm_ntfn, NULL);     /* park until armed */
        uint64_t last = fabwarm_now();
        while (g_fabwarm_armed) {
            uint64_t now = fabwarm_now();    /* cheap reg read, no syscall */
            if (now - last >= interval) {
                fabric_touch();
                g_fabwarm_iters++;
                last = now;
            }
        }
    }
}

/* Spawn the keep-warm thread (boot thread: vka/vspace are single-owner, so the
 * one allocation happens HERE). Inert until armed via /proc/fabwarm.1. */
void fabric_warm_start(void)
{
    vka_object_t nobj;
    if (vka_alloc_notification(&vka, &nobj)) {
        printf("[fabwarm] no ntfn -- keep-warm unavailable\n");
        return;
    }
    g_fabwarm_ntfn = nobj.cptr;

    sel4utils_thread_t th;
    if (sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
            simple_get_cnode(&simple), seL4_NilData, &th)) {
        printf("[fabwarm] configure failed -- keep-warm unavailable\n");
        return;
    }
    seL4_TCB_SetPriority(th.tcb.cptr, simple_get_tcb(&simple), 1);   /* lowest non-idle */
#if CONFIG_MAX_NUM_NODES > 1
    seL4_TCB_SetAffinity(th.tcb.cptr, 1);                            /* core 1: idle-only, never starves core 0 */
#endif
    sel4utils_start_thread(&th, fabric_warm_fn, NULL, NULL, 1);
    aios_acct_register("fabwarm", th.tcb.cptr);

    /* v0.4.269: DEFAULT-OFF (reverted from the v0.4.267 default-on). The GPLEV0
     * keep-warm is REFUTED -- a Device read crosses the ARM->VC AXI link but never
     * warms the cluster SCB snoop/DVM interface core 0's teardown dsb waits on
     * (project_stall_hunt: every CPU keep-warm refuted on HW). Leaving it ARMED also
     * kept core 1 busy-spinning in the ROOT vspace (asid 1), which marked residency[1]
     * bit 1 -- one of the {1,2,3} bits that made every asid=1 teardown IPI-storm the
     * idle siblings (the freeze the residency mask now neutralizes). Disarmed, core 1
     * returns to the idle thread (global vspace), so the residency mask over cores 1-3
     * is provably safe and the heartbeat can show core 1 surviving a core-0 stall.
     * /proc/fabwarm.1 still arms it for an A/B. */
    printf("[fabwarm] SCB-fabric keep-warm available on core 1 (default-OFF, refuted; /proc/fabwarm.1 to A/B)\n");
}

/* /proc/fabwarm[.0|.1] -- the live A/B knob. .1 = arm (core 1 keeps the fabric
 * warm), .0 = disarm (core 1 returns to idle), bare = status. */
int fabric_warm_cmd(const char *args, char *buf, int bufsize)
{
    if (args[0] == '.' && (args[1] == '1' || args[1] == '2')) {
        g_fabwarm_mode = (args[1] == '2') ? 2 : 1;
        g_fabwarm_armed = 1;
        arch_dsb();                                  /* publish the flags before the wake */
        if (g_fabwarm_ntfn) {
            seL4_Signal(g_fabwarm_ntfn);
        }
        return snprintf(buf, bufsize,
            "fabwarm: ARMED mode=%d (%s) -- core 1 generating SCB fabric traffic\n",
            g_fabwarm_mode, g_fabwarm_mode == 2 ? "GENET MDIO" : "GPLEV0");
    }
    if (args[0] == '.' && args[1] == '0') {
        g_fabwarm_armed = 0;
        arch_dsb();
        return snprintf(buf, bufsize, "fabwarm: disarmed -- core 1 returning to idle\n");
    }
    return snprintf(buf, bufsize,
        "fabwarm: armed=%d mode=%d iters=%llu gpio=%p genet=%p (.1 GPLEV0 / .2 GENET-MDIO / .0 disarm)\n",
        g_fabwarm_armed, g_fabwarm_mode, (unsigned long long)g_fabwarm_iters,
        (void *)dev_gpio_vaddr, (void *)dev_genet_vaddr);
}

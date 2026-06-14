/*
 * v3d.c -- Broadcom V3D 4.2 GPU driver, Phase 0 (power + IDENT + IRQ).
 *
 * RPi4 hardware-3D bring-up. Phase 0 proves ONLY the power story:
 *   - v3d_init():  claim the pre-mapped MMIO (hub+core, RPiVid ASB), bind GIC
 *     SPI 74 (seL4 IRQ 106) left fully MASKED, pin the VC mailbox tag buffer.
 *     ZERO V3D register access at boot -- a gated-block access can SError the
 *     A72, so the power sequence runs LAZILY, never in the boot path.
 *   - v3d_ensure_on() (via /proc/v3d.power): the HW-proven BCM2711 sequence --
 *     PM_GRAFX V3DRSTN deassert + RPiVid ASB master/slave un-stop -- then read
 *     HUB_IDENT0 and expect it to flip from the dead-bus poison 0xDEADBEEF to
 *     the magic 0x04443356. Aborts (v3d_ok=0) on mismatch -- never proceeds
 *     against a dead bus.
 *   - v3d_diag_cmd(): the /proc/v3d[.power[.N]|.r|.c|.w|.clock] netconsole
 *     probes that validate all of the above without a reflash.
 *
 * Every wait goes through v3d_wait() (cntpct_el0 deadline, NEVER an iteration
 * count -- the v0.4.176 eMMC 32.6s lesson). No MMU, no control lists, no
 * rendering: those are Phases 1-3 (docs/DESIGN_V3D_IMPLEMENTATION.md).
 */
#include "aios/root_shared.h"      /* vka, vspace, simple globals */
#include "aios/fb_console.h"        /* fb_console_set_suspend / fb_console_clear (FB ownership) */
#include "aios/device_map.h"       /* dev_v3d_vaddr, dev_v3d_asb_vaddr, dev_pm_vaddr, dev_vcmbox_* */
#include "aios/hw_info.h"          /* hw_info.has_v3d / v3d_paddr / v3d_irq */
#include "aios/mono_wait.h"        /* mono_deadline_ms / mono_before / mono_ticks */
#include <sel4/sel4.h>
#include <vka/object.h>
#include <sel4platsupport/device.h>
#include <stdio.h>
#include "arch.h"                  /* arch_dmb / arch_dsb */
#include "v3d_regs.h"
#include "v3d.h"
#include "v3d_cl.h"
#include "v3d_shaders.h"           /* the 3 QPU blobs + triangle geometry (Phase 3) */
#define LOG_MODULE "v3d"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "aios/aios_log.h"

/* ---- module state ---- */
static int      v3d_present;                /* 1 once MMIO is claimed (RPi4 with a V3D node) */
static int      v3d_ok_state;               /* 1 after a good power+IDENT probe */
static volatile uint32_t *v3d_base;         /* hub + core0, 8 pages (dev_v3d_vaddr) */
static volatile uint32_t *v3d_asb;          /* RPiVid ASB, 1 page (dev_v3d_asb_vaddr) */

/* IRQ 106 -- bound MASKED; Phase 0 invariant is that the counter stays 0. */
static seL4_CPtr v3d_irq_handler;
static seL4_CPtr v3d_irq_ntfn;              /* BASE (unbadged) cap -- polled for signals */
static int       v3d_irq_num = -1;
static volatile uint32_t v3d_irqs;
#define V3D_IRQ_BADGE  0x76u                /* nonzero so a fired IRQ is countable */

/* last-probe results, surfaced by `cat /proc/v3d` */
static uint32_t  v3d_ident0_last = V3D_BUS_POISON;
static uint32_t  v3d_power_count;
static uint64_t  v3d_asb_m_us, v3d_asb_s_us;
static int       v3d_asb_m_to, v3d_asb_s_to, v3d_asb_timeout;

/* ---- Phase 1: GPU memory pool + MMU (design §4) ---- */
#define V3D_POOL_BITS     23                 /* 8 MB untyped (phys-contiguous)        */
#define V3D_POOL_SIZE     (1u << V3D_POOL_BITS)
#define V3D_POOL_FRAMES   4                  /* 4 x 2 MB large pages                  */
#define V3D_PT_OFF        0x000000u          /* 4 MB page table (all-invalid = zeroed)*/
#define V3D_PT_BYTES      0x400000u
#define V3D_SCRATCH_OFF   0x400000u          /* 4 KB illegal-addr sink                */
#define V3D_BUMP_OFF      0x401000u          /* CLs / records / vertices (Phase 2+)   */
/* GPU VA layout (single 4 GB space; VA 0 never mapped). */
#define V3D_VA_DATA       0x00100000u        /* -> pool+SCRATCH (scratch + bump)      */
#define V3D_VA_FAULT      0x20000000u        /* deliberately UNMAPPED (Phase 1 probe) */
#define V3D_VA_FB         0x10000000u        /* live scanout FB mapped here (Phase 2) */

static uint8_t  *v3d_pool_va;                /* CPU base of the 8 MB pool (non-cacheable) */
static uint64_t  v3d_pool_pa;                /* phys base (2 MB aligned, contiguous)  */
static volatile uint32_t *v3d_pt;            /* = pool_va + V3D_PT_OFF (1M PTEs)       */
static uint64_t  v3d_scratch_pa;             /* = pool_pa + V3D_SCRATCH_OFF            */
static int       v3d_mmu_inited;

/* Phase 1 probe results (surfaced via /proc/v3d). */
static uint32_t  v3d_fault_vio_addr, v3d_fault_vio_id;
static uint32_t  v3d_mmu_faults_seen;

/* ---- Phase 2: the GPU kick (clear job + recovery + pixel probe) ----
 * The clear runs on the display_server thread (the single FB writer). The fs-thread
 * /proc/v3d.test verb posts g_v3d_req + signals disp_wake_ntfn_cap; display_server's
 * bound-notification Recv wakes, calls v3d_service_display_request() -> the takeover +
 * v3d_submit_frame + pixel probe (all on its own thread). Result lands in v3d_test,
 * harvested by the .test poll or a later `cat /proc/v3d`. */
static volatile uint32_t g_v3d_req;          /* request kind: 1 = clear, 2 = triangle */
static volatile uint32_t g_v3d_req_color;    /* FB-order pixel value to clear to */
static volatile uint32_t v3d_test_seq;       /* bumped after each job completes (poll) */
static uint32_t v3d_tests_run;               /* total jobs attempted */
static uint32_t v3d_resets;                  /* dump-and-reset count */

/* Last clear-job result + (on failure) the dump-and-reset register snapshot.
 * status: 0 ok, -1 bin timeout, -2 double-OUTOMEM, -3 MMU fault, -4 render timeout,
 *         -5 not powered / no FB. */
static struct {
    int      valid;
    int      status;
    uint32_t bfc0, bfc1, rfc0, rfc1;
    uint64_t bin_us, rend_us;
    int      oom;
    uint32_t color;
    uint32_t pixel;       /* probed center pixel (CleanInvalidate then read) */
    int      pixel_pass;
    uint32_t scratch0;    /* MMU scratch first word (R3 silent-redirect check) */
    int      is_tri;      /* 1 = triangle job (center != clear is PASS); 0 = clear */
    uint32_t corner_tl, corner_br;  /* triangle: a couple corner samples (visual aid) */
    /* dump-and-reset snapshot (valid when status != 0) */
    uint32_t ct0cs, ct1cs, ct0ca, ct1ca, ct0ra, ct1ra, ct0qba, ct1qba;
    uint32_t hub_ist, core_ist, vio_addr, vio_id, dbg, pt_base;
} v3d_test;

/* ---- register accessors (device memory; explicit barriers) ---- */
static inline uint32_t v3d_rd(uint32_t off)        { arch_dmb(); return v3d_base[off >> 2]; }
static inline void     v3d_wr(uint32_t off, uint32_t v) { v3d_base[off >> 2] = v; arch_dsb(); }
static inline uint32_t asb_rd(uint32_t off)        { arch_dmb(); return v3d_asb[off >> 2]; }
static inline void     asb_wr(uint32_t off, uint32_t v) { v3d_asb[off >> 2] = v; arch_dsb(); }

/* ---- timing helpers (cntpct_el0; the ONLY wait primitive) ---- */
static uint64_t mono_us_since(uint64_t t0) {
    uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (!f) f = 54000000;
    return (mono_ticks() - t0) * 1000000ull / f;
}
static void v3d_udelay(int us) {
    uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (!f) f = 54000000;
    uint64_t end = mono_ticks() + (uint64_t)us * f / 1000000ull;
    while (mono_ticks() < end) { /* sub-microsecond settle; far too short to yield */ }
}
/* The one wait helper: cond() != 0 within ms -> 1, else 0 on deadline. No raw
 * loops, no iteration counts anywhere else in this driver (grep-auditable). */
static int v3d_wait(int (*cond)(void), int ms) {
    for (uint64_t dl = mono_deadline_ms(ms); mono_before(dl); ) {
        arch_dmb();
        if (cond()) return 1;
    }
    arch_dmb();
    return cond();
}

/* ---- tiny arg parsers (hex for register offsets/values, dec for MHz/step) ---- */
static uint32_t v3d_hex(const char *p) {
    uint32_t v = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    for (;;) {
        char c = *p++; uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        v = v * 16u + d;
    }
    return v;
}
static uint32_t v3d_dec(const char *p) {
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10u + (uint32_t)(*p++ - '0');
    return v;
}
static int v3d_pfx(const char *s, const char *pfx) {   /* 1 if s starts with pfx */
    while (*pfx) { if (*s++ != *pfx++) return 0; }
    return 1;
}

/* ---- IRQ (bound masked; the counter must stay 0 in Phase 0) ---- */
static void v3d_bind_irq(void) {
    vka_object_t ntfn;
    if (vka_alloc_notification(&vka, &ntfn)) { AIOS_LOG_WARN("V3D IRQ ntfn alloc failed"); return; }

    /* Mint a BADGED copy for the IRQHandler: an unbadged IRQ signal delivers
     * badge 0, indistinguishable from "no signal", so the counter would be
     * unmeasurable. We poll the BASE cap, which still receives the badged signal. */
    cspacepath_t src, dst;
    vka_cspace_make_path(&vka, ntfn.cptr, &src);
    if (vka_cspace_alloc_path(&vka, &dst)) { AIOS_LOG_WARN("V3D IRQ mint path failed"); return; }
    if (seL4_CNode_Mint(dst.root, dst.capPtr, dst.capDepth, src.root, src.capPtr, src.capDepth,
                        seL4_AllRights, V3D_IRQ_BADGE)) {
        AIOS_LOG_WARN("V3D IRQ mint failed"); vka_cspace_free(&vka, dst.capPtr); return;
    }
    cspacepath_t hpath;
    if (vka_cspace_alloc_path(&vka, &hpath)) { AIOS_LOG_WARN("V3D IRQ cspace alloc failed"); return; }
    if (simple_get_IRQ_handler(&simple, hw_info.v3d_irq, hpath)) {
        AIOS_LOG_WARN_V("V3D IRQ handler get failed irq=", hw_info.v3d_irq); return;
    }
    v3d_irq_handler = hpath.capPtr;
    if (seL4_IRQHandler_SetNotification(v3d_irq_handler, dst.capPtr)) {
        AIOS_LOG_WARN("V3D IRQ SetNotification failed"); v3d_irq_handler = 0; return;
    }
    seL4_IRQHandler_Ack(v3d_irq_handler);
    v3d_irq_ntfn = ntfn.cptr;
    v3d_irq_num = (int)hw_info.v3d_irq;
    printf("[v3d] IRQ %d bound + masked (Phase 0: counter must stay 0)\n", v3d_irq_num);
}
/* Non-blocking sample. Nothing else consumes this ntfn, so in poll mode a
 * pending signal is ours; with everything masked there should be none. */
static void v3d_sample_irq(void) {
    if (!v3d_irq_ntfn) return;
    seL4_Word badge = 0;
    seL4_Poll(v3d_irq_ntfn, &badge);
    if (badge) { v3d_irqs++; if (v3d_irq_handler) seL4_IRQHandler_Ack(v3d_irq_handler); }
}

/* Read + decode the IDENT registers, set v3d_ok_state, and write ONE greppable
 * PASS/FAIL line. Powered = hub IDENT0 reads "VHUB" AND core0 IDENT0 reads
 * "V3D\004" (HW reality -- the design doc put the V3D\004 magic on the hub by
 * mistake; it is the core register). Pure reads, no writes -- safe on the
 * summary path too. `before`/show_before render the 0xDEADBEEF -> live
 * transition for the .power verb. Shared by .power and the /proc/v3d summary. */
static int v3d_report_ident(char *buf, int bufsize, uint32_t before, int show_before) {
    uint32_t hid0 = v3d_rd(V3D_HUB_IDENT0);
    uint32_t cid0 = v3d_rd(V3D_CORE0_OFFSET + V3D_CTL_IDENT0);
    v3d_ident0_last = hid0;
    if (hid0 == V3D_HUB_IDENT0_MAGIC && cid0 == V3D_CORE_IDENT0_MAGIC) {
        uint32_t id1  = v3d_rd(V3D_HUB_IDENT1);
        uint32_t id2  = v3d_rd(V3D_HUB_IDENT2);
        uint32_t cid1 = v3d_rd(V3D_CORE0_OFFSET + V3D_CTL_IDENT1);
        v3d_ok_state = 1;
        int n = 0;
        if (show_before)
            n += snprintf(buf, bufsize, "hub_ident0 0x%08x -> 0x%08x(VHUB) ", before, hid0);
        else
            n += snprintf(buf, bufsize, "hub_ident0 0x%08x(VHUB) ", hid0);
        n += snprintf(buf + n, bufsize - n,
            "core_ident0 0x%08x(V3D4) ver=%d.%d cores=%d qpus=%d mmu=%d PASS\n", cid0,
            V3D_HUB_IDENT1_TVER(id1), V3D_HUB_IDENT1_REV(id1), V3D_HUB_IDENT1_NCORES(id1),
            (int)(V3D_CTL_IDENT1_NSLC(cid1) * V3D_CTL_IDENT1_QUPS(cid1)),
            (id2 & V3D_HUB_IDENT2_WITH_MMU) ? 1 : 0);
        return n;
    }
    v3d_ok_state = 0;
    const char *hint = (hid0 == V3D_BUS_POISON)      ? "not powered -- run: cat /proc/v3d.power"
                     : (hid0 == 0)                   ? "0x0 (mapping bug? check prealloc log)"
                     : (hid0 == V3D_HUB_IDENT0_MAGIC)? "hub up, core0 IDENT0 wrong (partial power)"
                     :                                 "unexpected hub IDENT0";
    if (show_before)
        return snprintf(buf, bufsize, "hub_ident0 0x%08x -> 0x%08x core_ident0 0x%08x FAIL -- %s\n",
                        before, hid0, cid0, hint);
    return snprintf(buf, bufsize, "hub_ident0 0x%08x core_ident0 0x%08x FAIL -- %s\n",
                    hid0, cid0, hint);
}

/* ============================================================ *
 *  PM / ASB / mailbox -- the platform-specific hardware pokes  *
 * ============================================================ */
#ifdef PLAT_RPI4

/* ---- ASB un-stop: clear REQ_STOP, poll ACK clear (returns ACK-clear us) ---- */
static uint32_t g_asb_ctrl;
static int asb_ack_clear(void) { return !(asb_rd(g_asb_ctrl) & V3D_ASB_ACK); }
static uint64_t v3d_asb_unstop(uint32_t ctrl_off, int *timeout) {
    uint32_t v = asb_rd(ctrl_off);
    asb_wr(ctrl_off, V3D_PM_PASSWORD | (v & ~V3D_ASB_REQ_STOP));   /* password OR'd in */
    g_asb_ctrl = ctrl_off;
    uint64_t t0 = mono_ticks();
    int ok = v3d_wait(asb_ack_clear, 10);
    uint64_t us = mono_us_since(t0);
    *timeout = !ok;
    if (!ok) v3d_asb_timeout = 1;
    return us;
}

/* The BCM2711 V3D power-on. only_step==0 runs the whole sequence in order;
 * only_step 1..4 runs ONLY that micro-step, for SError bisection over
 * netconsole (/proc/v3d.power.N). Sets v3d_ok_state. Appends a human log. */
static int v3d_power_seq(int only_step, char *buf, int bufsize) {
    int  all = (only_step == 0);
    int  w   = 0;
    uint32_t before = 0;

    if (all) {
        before = v3d_rd(V3D_HUB_IDENT0);            /* pre-power READS are safe */
        w += snprintf(buf + w, bufsize - w, "ident0 before: 0x%08x\n", before);
    }

    /* step 1: PM_GRAFX V3DRSTN deassert (+ >=2us settle). Password in EVERY write. */
    if (all || only_step == 1) {
        uint32_t pm = dev_pm_vaddr[V3D_PM_GRAFX / 4];
        dev_pm_vaddr[V3D_PM_GRAFX / 4] = V3D_PM_PASSWORD | (pm | V3D_PM_V3DRSTN);
        arch_dsb();
        v3d_udelay(5);                               /* Linux waits 1us; design wants >=2us */
        v3d_power_count++;
        w += snprintf(buf + w, bufsize - w, "step1 PM_GRAFX V3DRSTN: 0x%08x -> 0x%08x\n",
                      pm, dev_pm_vaddr[V3D_PM_GRAFX / 4]);
    }
    /* step 2: ASB MASTER un-stop, poll ACK clear (10ms). */
    if (all || only_step == 2) {
        v3d_asb_m_us = v3d_asb_unstop(V3D_ASB_M_CTRL, &v3d_asb_m_to);
        w += snprintf(buf + w, bufsize - w, "step2 ASB master ack=%lluus%s\n",
                      (unsigned long long)v3d_asb_m_us, v3d_asb_m_to ? " TIMEOUT" : "");
    }
    /* step 3: ASB SLAVE un-stop, poll ACK clear (10ms). */
    if (all || only_step == 3) {
        v3d_asb_s_us = v3d_asb_unstop(V3D_ASB_S_CTRL, &v3d_asb_s_to);
        w += snprintf(buf + w, bufsize - w, "step3 ASB slave  ack=%lluus%s\n",
                      (unsigned long long)v3d_asb_s_us, v3d_asb_s_to ? " TIMEOUT" : "");
    }
    /* step 4: read + decode IDENT (the actual probe). */
    if (all || only_step == 4) {
        w += v3d_report_ident(buf + w, bufsize - w, before, all);
        if (v3d_ok_state) {
            /* force-mask both interrupt blocks now that the block is powered
             * (keeps the Phase 0 IRQ-counter-stays-0 invariant explicit). */
            v3d_wr(V3D_HUB_INT_MSK_SET, 0xffffffffu);
            v3d_wr(V3D_CORE0_OFFSET + V3D_CTL_INT_MSK_SET, 0xffffffffu);
        }
    }
    return w;
}

/* ---- VC property mailbox, tag buffer pinned at 0x3A002000 (clone of
 *      pcie_brcmstb.c, but with V3D's own buffer in the GPU reserved region's
 *      forward-only watermark, after display 0x3A000000 + PCIe 0x3A001000). ---- */
#define V3D_MBOX_TAG_PADDR  0x3A002000UL
#define MBOX_READ_OFF       0x00
#define MBOX_STATUS_OFF     0x18
#define MBOX_WRITE_OFF      0x20
#define MBOX_FULL           0x80000000U
#define MBOX_EMPTY          0x40000000U
#define MBOX_CH_PROP        8
#define MBOX_RESP_OK        0x80000000U
#define VC_TAG_GET_CLOCK_RATE      0x00030002u
#define VC_TAG_GET_MAX_CLOCK_RATE  0x00030004u
#define VC_TAG_SET_CLOCK_RATE      0x00038002u
#define VC_TAG_GET_TEMPERATURE     0x00030006u
#define VC_TAG_GET_MAX_TEMPERATURE 0x0003000Au
#define VC_CLOCK_ID_ARM        3u
#define VC_CLOCK_ID_V3D        5u

static volatile uint32_t *v3d_mbox_regs;
static volatile uint32_t *v3d_mbox_buf;
static uint64_t v3d_mbox_buf_bus;
static int v3d_mbox_ready;

static void v3d_mbox_init(void) {
    if (v3d_mbox_ready) return;
    if (!dev_vcmbox_vaddr) { AIOS_LOG_WARN("V3D: VC mbox not mapped (.clock unavailable)"); return; }
    v3d_mbox_regs = (volatile uint32_t *)((uintptr_t)dev_vcmbox_vaddr + dev_vcmbox_off);
    vka_object_t f;
    if (sel4platsupport_alloc_frame_at(&vka, V3D_MBOX_TAG_PADDR, seL4_PageBits, &f)) {
        AIOS_LOG_WARN("V3D: tag buffer pin @0x3A002000 failed (.clock unavailable)"); return;
    }
    void *v = vspace_map_pages(&vspace, &f.cptr, NULL, seL4_AllRights, 1, seL4_PageBits, 0);
    if (!v) { AIOS_LOG_WARN("V3D: tag buffer map failed"); return; }
    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(f.cptr);
    if (ga.error) { AIOS_LOG_WARN("V3D: tag buffer paddr failed"); return; }
    v3d_mbox_buf = (volatile uint32_t *)v;
    v3d_mbox_buf_bus = ga.paddr | 0xC0000000ULL;   /* VC bus alias of the low pin */
    v3d_mbox_ready = 1;
}
static int v3d_mbox_send(void) {
    uint32_t addr_ch = (uint32_t)(v3d_mbox_buf_bus & 0xFFFFFFF0U) | MBOX_CH_PROP;
    for (uint64_t dl = mono_deadline_ms(2000); mono_before(dl); ) {
        arch_dmb();
        if (!(v3d_mbox_regs[MBOX_STATUS_OFF / 4] & MBOX_FULL)) break;
    }
    arch_dsb();
    v3d_mbox_regs[MBOX_WRITE_OFF / 4] = addr_ch;
    arch_dsb();
    for (uint64_t dl = mono_deadline_ms(2000); mono_before(dl); ) {
        arch_dmb();
        if (v3d_mbox_regs[MBOX_STATUS_OFF / 4] & MBOX_EMPTY) continue;
        arch_dmb();
        if (v3d_mbox_regs[MBOX_READ_OFF / 4] == addr_ch) {
            arch_dmb();
            return (v3d_mbox_buf[1] == MBOX_RESP_OK) ? 0 : -1;
        }
    }
    return -1;
}
static int v3d_vc_tag_raw(uint32_t tag, uint32_t *vals, int nwords) {
    if (!v3d_mbox_ready) { v3d_mbox_init(); if (!v3d_mbox_ready) return -1; }
    volatile uint32_t *b = v3d_mbox_buf;
    b[0] = (uint32_t)((6 + nwords) * 4);
    b[1] = 0;
    b[2] = tag;
    b[3] = (uint32_t)(nwords * 4);
    b[4] = 0;
    for (int k = 0; k < nwords; k++) b[5 + k] = vals[k];
    b[5 + nwords] = 0;
    arch_dsb();
    if (v3d_mbox_send()) return -1;
    for (int k = 0; k < nwords; k++) vals[k] = b[5 + k];
    return 0;
}
/* The VC mailbox tag buffer is shared but is now Called from TWO threads: the fs
 * thread (/proc/temp, /proc/cpufreq, /proc/v3d) and the root main thread (the DVFS
 * governor via hw_arm_clock_set). Serialize with a non-blocking test-and-set so a
 * concurrent caller cannot corrupt a transaction mid-flight; the loser gets -1
 * (treated as "mailbox unavailable", which both callers already handle -- the
 * governor skips the tick, a /proc read shows unavailable). */
static volatile unsigned char vc_lock = 0;
static int v3d_vc_tag(uint32_t tag, uint32_t *vals, int nwords) {
    if (__atomic_test_and_set(&vc_lock, __ATOMIC_ACQUIRE)) return -1;
    int rc = v3d_vc_tag_raw(tag, vals, nwords);
    __atomic_clear(&vc_lock, __ATOMIC_RELEASE);
    return rc;
}
static int v3d_clock_verb(int set, uint32_t mhz, char *buf, int bufsize) {
    if (set) {
        uint32_t v[3] = { VC_CLOCK_ID_V3D, mhz * 1000000u, 0 };
        if (v3d_vc_tag(VC_TAG_SET_CLOCK_RATE, v, 3))
            return snprintf(buf, bufsize, "v3d: SET_CLOCK_RATE(%u MHz) failed (non-fatal)\n", mhz);
        return snprintf(buf, bufsize, "v3d: clock set -> %u Hz (id=5)\n", v[1]);
    }
    uint32_t v[2] = { VC_CLOCK_ID_V3D, 0 };
    if (v3d_vc_tag(VC_TAG_GET_CLOCK_RATE, v, 2))
        return snprintf(buf, bufsize, "v3d: GET_CLOCK_RATE failed (non-fatal)\n");
    return snprintf(buf, bufsize, "v3d: clock = %u Hz (id=5)\n", v[1]);
}

/* Live SoC readouts over the SAME VC mailbox (reused by /proc/cpufreq + /proc/temp).
 * They live here because v3d.c owns the HW-verified, low-pinned tag-buffer mailbox
 * path; the readouts are not V3D-specific. Fill *cur (and *max if non-NULL); return
 * 0 on success, -1 if the mailbox is unavailable. */
int hw_arm_clock_hz(unsigned int *cur_hz, unsigned int *max_hz) {
    uint32_t v[2] = { VC_CLOCK_ID_ARM, 0 };
    if (v3d_vc_tag(VC_TAG_GET_CLOCK_RATE, v, 2)) return -1;
    if (cur_hz) *cur_hz = v[1];
    if (max_hz) {
        uint32_t m[2] = { VC_CLOCK_ID_ARM, 0 };
        *max_hz = v3d_vc_tag(VC_TAG_GET_MAX_CLOCK_RATE, m, 2) ? 0u : m[1];
    }
    return 0;
}
int hw_soc_temp_mc(int *cur_mc, int *max_mc) {
    uint32_t v[2] = { 0u, 0u };   /* temperature id 0 */
    if (v3d_vc_tag(VC_TAG_GET_TEMPERATURE, v, 2)) return -1;
    if (cur_mc) *cur_mc = (int)v[1];
    if (max_mc) {
        uint32_t m[2] = { 0u, 0u };
        *max_mc = v3d_vc_tag(VC_TAG_GET_MAX_TEMPERATURE, m, 2) ? 0 : (int)m[1];
    }
    return 0;
}
/* Set the ARM core clock via the VC mailbox -- the RPi4 DVFS power lever (the
 * only one compatible with the no-WFI stall cure; core-parking re-triggers the
 * 32.4s tlbi stall). The firmware clamps the request to its allowed range
 * ([arm_freq_min, arm_freq] from config.txt) and returns the actual rate set in
 * *new_hz. IMPORTANT: cntpct_el0 (the generic timer) runs at a FIXED rate
 * independent of this clock, so lowering the ARM clock slows computation but does
 * NOT break timeouts / scheduling. Returns 0 on success, -1 if the mailbox is
 * unavailable. */
int hw_arm_clock_set(unsigned int mhz, unsigned int *new_hz) {
    uint32_t v[3] = { VC_CLOCK_ID_ARM, mhz * 1000000u, 0 };  /* [2]=0: clamp turbo */
    if (v3d_vc_tag(VC_TAG_SET_CLOCK_RATE, v, 3)) return -1;
    if (new_hz) *new_hz = v[1];   /* firmware returns the actual rate it set */
    return 0;
}

#else  /* !PLAT_RPI4 -- QEMU has no V3D model; these are never reached (has_v3d=0) */

static void v3d_mbox_init(void) {}
int hw_arm_clock_hz(unsigned int *c, unsigned int *m) { (void)c; (void)m; return -1; }
int hw_soc_temp_mc(int *c, int *m) { (void)c; (void)m; return -1; }
int hw_arm_clock_set(unsigned int m, unsigned int *n) { (void)m; (void)n; return -1; }
static int  v3d_power_seq(int only_step, char *buf, int bufsize) {
    (void)only_step;
    return snprintf(buf, bufsize, "v3d: power sequence unavailable (not RPi4)\n");
}
static int  v3d_clock_verb(int set, uint32_t mhz, char *buf, int bufsize) {
    (void)set; (void)mhz;
    return snprintf(buf, bufsize, "v3d: clock unavailable (not RPi4)\n");
}

#endif /* PLAT_RPI4 */

/* ============================================================ *
 *  Phase 1 -- GPU memory pool + MMU (page table, init, fault)   *
 * ============================================================ */

/* Reserve the 8 MB GPU pool: one phys-contiguous untyped retyped into 4 x 2 MB
 * large pages, mapped NON-CACHEABLE (xHCI/GENET DMA philosophy -- coherent by
 * construction). Call EARLY (after xhci_dma_reserve, before fs/display eat low
 * RAM) so an 8 MB contiguous untyped is still available (design risk #7).
 * Idempotent; safe to call when has_v3d == 0 (it just reserves RAM). 0 on success. */
int v3d_mem_reserve(void) {
    if (v3d_pool_va) return 0;
    if (!hw_info.has_v3d) return 0;            /* no GPU -> don't waste 8 MB */
    vka_object_t ut;
    if (vka_alloc_untyped(&vka, V3D_POOL_BITS, &ut)) {
        AIOS_LOG_ERROR("V3D: 8MB pool untyped alloc failed (VKA pressure?)");
        return -1;
    }
    seL4_CPtr frames[V3D_POOL_FRAMES];
    for (int i = 0; i < V3D_POOL_FRAMES; i++) {
        seL4_CPtr slot;
        if (vka_cspace_alloc(&vka, &slot)) { AIOS_LOG_ERROR("V3D: pool cslot alloc failed"); return -1; }
        /* sequential retypes from ONE untyped -> the 4 frames are contiguous */
        if (seL4_Untyped_Retype(ut.cptr, seL4_ARM_LargePageObject, seL4_LargePageBits,
                                seL4_CapInitThreadCNode, 0, 0, slot, 1)) {
            AIOS_LOG_ERROR_V("V3D: pool retype failed at frame ", (unsigned long)i);
            return -1;
        }
        frames[i] = slot;
    }
    void *va = vspace_map_pages(&vspace, frames, NULL, seL4_AllRights,
                                V3D_POOL_FRAMES, seL4_LargePageBits, 0 /* non-cacheable */);
    if (!va) { AIOS_LOG_ERROR("V3D: pool map failed"); return -1; }
    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(frames[0]);
    if (ga.error) { AIOS_LOG_ERROR("V3D: pool GetAddress failed"); return -1; }
    v3d_pool_va     = (uint8_t *)va;
    v3d_pool_pa     = ga.paddr;
    v3d_pt          = (volatile uint32_t *)(v3d_pool_va + V3D_PT_OFF);
    v3d_scratch_pa  = v3d_pool_pa + V3D_SCRATCH_OFF;
    printf("[v3d] pool: 8MB @ phys 0x%lx (va %p) PT@+0 scratch@+0x400000\n",
           (unsigned long)v3d_pool_pa, va);
    return 0;
}

#ifdef PLAT_RPI4
/* ---- Phase 2: BO bump allocator over the pool bump region ----
 * The data window maps pool+SCRATCH onward at V3D_VA_DATA, so the GPU VA of a
 * pool offset is V3D_VA_DATA + (off - V3D_SCRATCH_OFF). Reset per submitted frame. */
struct v3d_bo { uint8_t *cpu; uint64_t pa; uint32_t gpu_va; uint32_t size; };
static uint32_t v3d_bump_off = V3D_BUMP_OFF;
static void v3d_bo_reset_frame(void) { v3d_bump_off = V3D_BUMP_OFF; }
static int v3d_bo_alloc(struct v3d_bo *bo, uint32_t size, uint32_t align) {
    uint32_t off = (v3d_bump_off + (align - 1)) & ~(align - 1);
    if (off + size > V3D_POOL_SIZE) return -1;
    bo->cpu    = v3d_pool_va + off;
    bo->pa     = v3d_pool_pa + off;
    bo->gpu_va = V3D_VA_DATA + (off - V3D_SCRATCH_OFF);
    bo->size   = size;
    v3d_bump_off = off + size;
    return 0;
}

/* Two-step MMU flush (Linux v3d_mmu_flush_all order, deadline-bounded). */
static int v3d_mmuc_flush_clear(void) { return !(v3d_rd(V3D_MMUC_CONTROL) & V3D_MMUC_CONTROL_FLUSHING); }
static int v3d_tlb_clear_done(void)   { return !(v3d_rd(V3D_MMU_CTL) & V3D_MMU_CTL_TLB_CLEARING); }
static void v3d_mmu_flush_all(void) {
    v3d_wr(V3D_MMUC_CONTROL, V3D_MMUC_CONTROL_FLUSH | V3D_MMUC_CONTROL_ENABLE);
    v3d_wait(v3d_mmuc_flush_clear, 100);
    v3d_wr(V3D_MMU_CTL, v3d_rd(V3D_MMU_CTL) | V3D_MMU_CTL_TLB_CLEAR);
    v3d_wait(v3d_tlb_clear_done, 100);
}

/* Map npages 4 KB pages: GPU VA -> phys, writeable. PTE = flags | (pa >> 12).
 * The PT lives in the non-cacheable pool, so PTE writes need only a dsb. */
static void v3d_mmu_map(uint32_t gpu_va, uint64_t pa, uint32_t npages, int w) {
    uint32_t idx = gpu_va >> V3D_MMU_PAGE_SHIFT;
    uint32_t flags = V3D_PTE_VALID | (w ? V3D_PTE_WRITEABLE : 0);
    for (uint32_t i = 0; i < npages; i++)
        v3d_pt[idx + i] = flags | (uint32_t)((pa + ((uint64_t)i << V3D_MMU_PAGE_SHIFT)) >> V3D_MMU_PAGE_SHIFT);
    arch_dsb();
}

/* Build the page table (all-invalid), map the static regions, program the MMU in
 * Linux v3d_mmu_set_page_table order, flush. Requires the pool + a powered core. */
static int v3d_mmu_init(void) {
    if (v3d_mmu_inited) return 0;
    if (!v3d_pool_va) { AIOS_LOG_ERROR("V3D: mmu_init with no pool"); return -1; }
    if (!v3d_ok_state) { AIOS_LOG_WARN("V3D: mmu_init before power -- run .power first"); return -1; }

    /* 1. zero the 4 MB PT (every PTE invalid: a stray GPU VA -> PT_INVALID fault). */
    for (uint32_t i = 0; i < V3D_PT_BYTES / 4; i++) v3d_pt[i] = 0;
    arch_dsb();
    /* 2. map the data window (scratch + bump) at V3D_VA_DATA, writeable.
     *    0x20000000 is left UNMAPPED on purpose -- the Phase 1 fault target. */
    v3d_mmu_map(V3D_VA_DATA, v3d_pool_pa + V3D_SCRATCH_OFF,
                (V3D_POOL_SIZE - V3D_SCRATCH_OFF) >> V3D_MMU_PAGE_SHIFT, 1);
    /* 2b. map the live scanout framebuffer at V3D_VA_FB so the render CL can store
     *     pixels to it. gpu_fb_pa is firmware-chosen -- read live, never a constant. */
    if (gpu_available && gpu_fb_pa) {
        uint32_t fb_pages = ((gpu_width * gpu_height * 4u) + 0xfffu) >> V3D_MMU_PAGE_SHIFT;
        v3d_mmu_map(V3D_VA_FB, gpu_fb_pa, fb_pages, 1);
    }

    /* 3. program the MMU (exact Linux order + bit list). */
    v3d_wr(V3D_MMU_PT_PA_BASE, (uint32_t)(v3d_pool_pa >> V3D_MMU_PAGE_SHIFT));
    v3d_wr(V3D_MMU_CTL,
           V3D_MMU_CTL_ENABLE | V3D_MMU_CTL_PT_INVALID_ENABLE |
           V3D_MMU_CTL_PT_INVALID_ABORT | V3D_MMU_CTL_PT_INVALID_INT |
           V3D_MMU_CTL_WRITE_VIOLATION_ABORT | V3D_MMU_CTL_WRITE_VIOLATION_INT |
           V3D_MMU_CTL_CAP_EXCEEDED_ABORT | V3D_MMU_CTL_CAP_EXCEEDED_INT);
    v3d_wr(V3D_MMU_ILLEGAL_ADDR,
           (uint32_t)(v3d_scratch_pa >> V3D_MMU_PAGE_SHIFT) | V3D_MMU_ILLEGAL_ADDR_ENABLE);
    v3d_wr(V3D_MMUC_CONTROL, V3D_MMUC_CONTROL_ENABLE);
    v3d_mmu_flush_all();

    /* keep the hub INT masked (Phase 0 invariant); the PTI STATUS still latches. */
    v3d_wr(V3D_HUB_INT_MSK_SET, 0xffffffffu);
    v3d_mmu_inited = 1;
    return 0;
}

/* Best-effort: make sure the V3D CORE clock is running before we kick the CLE.
 * .power (PM_GRAFX + ASB) only un-stops the register/APB domain -- IDENT reads
 * work without a core clock, but the CLE thread won't advance (and thus won't
 * fetch -> won't fault) if the firmware left the V3D clock at 0. GET it; if 0,
 * SET a conservative 500 MHz; return the resulting Hz (0 = mailbox unavailable).
 * Non-fatal: the firmware clock path and our manual PM/ASB power coexist. */
static uint32_t v3d_ensure_clock(void) {
    uint32_t g[2] = { VC_CLOCK_ID_V3D, 0 };
    if (v3d_vc_tag(VC_TAG_GET_CLOCK_RATE, g, 2)) return 0;
    if (g[1] == 0) {
        uint32_t s[3] = { VC_CLOCK_ID_V3D, 500u * 1000000u, 0 };
        v3d_vc_tag(VC_TAG_SET_CLOCK_RATE, s, 3);
        g[0] = VC_CLOCK_ID_V3D; g[1] = 0;
        if (v3d_vc_tag(VC_TAG_GET_CLOCK_RATE, g, 2)) return 0;
    }
    return g[1];
}

/* /proc/v3d.mmu : build + program the MMU (no fault), report the key registers. */
static int v3d_mmu_verb(char *buf, int bufsize) {
    if (!v3d_ok_state) {
        char id[160]; v3d_report_ident(id, sizeof(id), 0, 0);
        if (!v3d_ok_state)
            return snprintf(buf, bufsize, "v3d.mmu: core not powered -- run cat /proc/v3d.power first\n");
    }
    if (v3d_mmu_init()) return snprintf(buf, bufsize, "v3d.mmu: init failed (see log)\n");
    return snprintf(buf, bufsize,
        "v3d.mmu: ON  PT@phys 0x%lx (%u PTEs)  scratch@0x%lx\n"
        "  PT_PA_BASE=0x%08x MMU_CTL=0x%08x MMUC_CONTROL=0x%08x ILLEGAL_ADDR=0x%08x\n"
        "  mapped: VA 0x%08x -> phys 0x%lx (%u pages, RW) ; VA 0x%08x left UNMAPPED\n",
        (unsigned long)v3d_pool_pa, V3D_PT_BYTES / 4, (unsigned long)v3d_scratch_pa,
        v3d_rd(V3D_MMU_PT_PA_BASE), v3d_rd(V3D_MMU_CTL),
        v3d_rd(V3D_MMUC_CONTROL), v3d_rd(V3D_MMU_ILLEGAL_ADDR),
        V3D_VA_DATA, (unsigned long)(v3d_pool_pa + V3D_SCRATCH_OFF),
        (V3D_POOL_SIZE - V3D_SCRATCH_OFF) >> V3D_MMU_PAGE_SHIFT, V3D_VA_FAULT);
}

/* /proc/v3d.fault : the Phase 1 probe. Kick the render thread (CT1) at the
 * deliberately-unmapped V3D_VA_FAULT, poll the HUB INT status for MMU_PTI (the
 * IRQ stays masked -- we read the latched status), capture VIO_ADDR/VIO_ID,
 * W1C-clear + flush (dump-and-reset), and confirm IDENT still reads. */
static int v3d_hub_pti(void) { return (v3d_rd(V3D_HUB_INT_STS) & V3D_HUB_INT_MMU_PTI) != 0; }
static int v3d_fault_probe(char *buf, int bufsize) {
    if (!v3d_ok_state) {
        char id[160]; v3d_report_ident(id, sizeof(id), 0, 0);
        if (!v3d_ok_state)
            return snprintf(buf, bufsize, "v3d.fault: core not powered -- run cat /proc/v3d.power first\n");
    }
    if (v3d_mmu_init()) return snprintf(buf, bufsize, "v3d.fault: mmu_init failed (see log)\n");
    uint32_t clk = v3d_ensure_clock();   /* the CLE needs the core clock to fetch */

    /* clear any stale hub MMU status, then kick CT1 at the unmapped VA. */
    v3d_wr(V3D_HUB_INT_CLR, V3D_HUB_INT_MMU_PTI | V3D_HUB_INT_MMU_WRV | V3D_HUB_INT_MMU_CAP);
    arch_dsb();
    v3d_wr(V3D_CORE0_OFFSET + V3D_CLE_CT1QBA, V3D_VA_FAULT);
    arch_dsb();
    v3d_wr(V3D_CORE0_OFFSET + V3D_CLE_CT1QEA, V3D_VA_FAULT + 0x100);   /* QEA write = kick */
    arch_dsb();

    int faulted = v3d_wait(v3d_hub_pti, 50);
    uint32_t sts  = v3d_rd(V3D_HUB_INT_STS);
    uint32_t vraw = v3d_rd(V3D_MMU_VIO_ADDR);
    uint32_t vid  = v3d_rd(V3D_MMU_VIO_ID);
    uint32_t dbg  = v3d_rd(V3D_MMU_DEBUG_INFO);
    uint32_t ct1ca = v3d_rd(V3D_CORE0_OFFSET + V3D_CLE_CT1CA);
    /* VIO_ADDR is the faulting GPU VA shifted right by (va_width - 32); va_width is
     * read from MMU_DEBUG_INFO, exactly as Linux v3d does (vio = reg << (va_width-32)).
     * HW-confirmed on bcm2711 V3D 4.2: DEBUG_INFO 0x550 -> va_width 35 -> shift 3, so
     * raw 0x04000018 << 3 = 0x200000c0 -> page 0x20000000 (the kicked VA). The old
     * <<8/<<12 byte guesses were both wrong. */
    uint32_t va_width = V3D_MMU_WIDTH_BASE +
                        ((dbg >> V3D_MMU_DEBUG_VA_WIDTH_SHIFT) & V3D_MMU_WIDTH_FIELD_MASK);
    uint32_t pa_width = V3D_MMU_WIDTH_BASE +
                        ((dbg >> V3D_MMU_DEBUG_PA_WIDTH_SHIFT) & V3D_MMU_WIDTH_FIELD_MASK);
    int vsh = (int)va_width - 32;
    uint64_t vio_full = (vsh >= 0) ? ((uint64_t)vraw << vsh) : ((uint64_t)vraw >> -vsh);
    uint32_t vio_page = (uint32_t)(vio_full & ~0xfffull);
    /* VIO_ID client id: for V3D >= 4.1 the client is id >> 5 (Linux v3d41_axi_ids).
     * Our CT1 fault is a CLE (control-list executor) fetch -- id 0x84 >> 5 = 4. */
    static const char *const v3d_axi_ids[8] = {
        "L2T", "PTB", "PSE", "TLB", "CLE", "TFU", "MMU", "GMP" };
    uint32_t cli = vid >> 5;
    const char *cliname = (cli < 8) ? v3d_axi_ids[cli] : "?";
    v3d_fault_vio_addr = vio_page; v3d_fault_vio_id = vid;
    if (faulted) v3d_mmu_faults_seen++;

    /* dump-and-reset: clear the latched hub MMU status + any core error the abort
     * raised, then flush the MMU so a second .fault re-kicks from a clean slate. */
    v3d_wr(V3D_HUB_INT_CLR, V3D_HUB_INT_MMU_PTI | V3D_HUB_INT_MMU_WRV | V3D_HUB_INT_MMU_CAP);
    v3d_wr(V3D_CORE0_OFFSET + V3D_CTL_INT_CLR, 0xffffffffu);
    v3d_mmu_flush_all();

    /* regression: IDENT must still read after the fault + reset. */
    char id[160]; v3d_report_ident(id, sizeof(id), 0, 0);
    int ident_ok = v3d_ok_state;

    int pass = faulted && ident_ok && vio_page == V3D_VA_FAULT;
    return snprintf(buf, bufsize,
        "v3d.fault: core_clk=%u Hz  kicked CT1 @ 0x%08x  hub_int_sts=0x%08x PTI=%d\n"
        "  va_width=%u pa_width=%u (DEBUG_INFO=0x%08x)\n"
        "  VIO_ADDR raw=0x%08x -> 0x%09llx page=0x%08x (<<%d)  client=%s VIO_ID=0x%08x  CT1CA=0x%08x\n"
        "  reset: flushed, ident_after=%s  -- %s\n"
        "  (no fault? try: cat /proc/v3d.clock.500 then re-run cat /proc/v3d.fault)\n",
        clk, V3D_VA_FAULT, sts, faulted ? 1 : 0,
        va_width, pa_width, dbg,
        vraw, (unsigned long long)vio_full, vio_page, vsh, cliname, vid, ct1ca,
        ident_ok ? "PASS" : "FAIL", pass ? "PASS" : "FAIL");
}
/* /proc/v3d.cl : Phase 2 dry-run. Map the FB, allocate the CL + tile buffers from
 * the pool via the BO allocator, build the bin + render clear CLs, and report
 * sizes + GPU VAs + first bytes (cross-check vs the host golden gate). The GPU is
 * NOT kicked and the framebuffer is NOT written -- pure construction + MMU map. */
static int v3d_cl_probe(char *buf, int bufsize) {
    if (!v3d_ok_state) {
        char id[160]; v3d_report_ident(id, sizeof(id), 0, 0);
        if (!v3d_ok_state)
            return snprintf(buf, bufsize, "v3d.cl: core not powered -- run cat /proc/v3d.power first\n");
    }
    if (v3d_mmu_init()) return snprintf(buf, bufsize, "v3d.cl: mmu_init failed (see log)\n");
    if (!gpu_available || !gpu_fb_pa)
        return snprintf(buf, bufsize, "v3d.cl: no framebuffer (gpu_available=%d fb_pa=0x%llx)\n",
                        gpu_available, (unsigned long long)gpu_fb_pa);

    v3d_bo_reset_frame();
    struct v3d_bo ts, ta, bcl, rcl;
    if (v3d_bo_alloc(&ts, 48u * 1024, 4096) || v3d_bo_alloc(&ta, 40u * 1024, 4096) ||
        v3d_bo_alloc(&bcl, 256, 128) || v3d_bo_alloc(&rcl, 4096, 128))
        return snprintf(buf, bufsize, "v3d.cl: BO alloc failed (pool exhausted)\n");

    struct v3d_clear_params cp = {
        .width = (uint16_t)gpu_width, .height = (uint16_t)gpu_height,
        .clear_color = 0xFFFF8000u, .fb_va = V3D_VA_FB,
        .fb_stride = gpu_width * 4u, .tile_alloc_va = ta.gpu_va,
        .rb_swap = 0,   /* AIOS FB is BGR (pixel-ord 0): store verbatim, no R/B swap */
    };
    int bn = v3d_build_bin_cl(bcl.cpu, (int)bcl.size, &cp);
    int rn = v3d_build_render_cl(rcl.cpu, (int)rcl.size, rcl.gpu_va, &cp);
    if (bn < 0 || rn < 0)
        return snprintf(buf, bufsize, "v3d.cl: emit failed (bn=%d rn=%d -- buffer too small)\n", bn, rn);

    uint32_t fb_pages = ((gpu_width * gpu_height * 4u) + 0xfffu) >> V3D_MMU_PAGE_SHIFT;
    int w = 0;
    w += snprintf(buf + w, bufsize - w,
        "v3d.cl: %ux%u clear=0x%08x  FB va=0x%08x pa=0x%llx (%u pages, mapped)\n",
        gpu_width, gpu_height, cp.clear_color, V3D_VA_FB,
        (unsigned long long)gpu_fb_pa, fb_pages);
    w += snprintf(buf + w, bufsize - w,
        "  tile_state va=0x%08x  tile_alloc va=0x%08x\n", ts.gpu_va, ta.gpu_va);
    w += snprintf(buf + w, bufsize - w, "  bin_cl  va=0x%08x len=%d :", bcl.gpu_va, bn);
    for (int i = 0; i < bn; i++) w += snprintf(buf + w, bufsize - w, " %02x", bcl.cpu[i]);
    w += snprintf(buf + w, bufsize - w, "\n  rend_cl va=0x%08x len=%d :", rcl.gpu_va, rn);
    for (int i = 0; i < rn && i < 24; i++) w += snprintf(buf + w, bufsize - w, " %02x", rcl.cpu[i]);
    w += snprintf(buf + w, bufsize - w,
        "\n  built OK, NOT kicked -- submission (bin/render kick) is the next step\n");
    return w;
}

/* ============================================================ *
 *  Phase 2 -- the GPU kick: submit + recovery + pixel probe     *
 *  (display_server thread only; FB ownership taken internally)  *
 * ============================================================ */

struct v3d_clear_result {
    uint32_t bfc0, bfc1, rfc0, rfc1;
    uint64_t bin_us, rend_us;
    int      oom;
};

/* Invalidate the GPU-internal caches before a kick so the CLE re-reads the CLs/PT we
 * just wrote (the non-cacheable pool guarantees CPU->RAM; this drops the GPU's own
 * stale slice/L2T lines from a prior job). Deadline-bounded on the L2T flush. */
static int v3d_l2t_flush_done(void) {
    return !(v3d_rd(V3D_CORE0_OFFSET + V3D_CTL_L2TCACTL) & V3D_L2TCACTL_L2TFLS);
}
/* Full-range L2T flush (clean+invalidate), deadline-bounded. Before a kick this
 * drops stale CL/PT lines; AFTER render it pushes the GPU's FB stores out to RAM so
 * scanout + the CPU pixel probe see them (Linux v3d_clean_caches readback path). */
static void v3d_l2t_flush(void) {
    v3d_wr(V3D_CORE0_OFFSET + V3D_CTL_L2TFLSTA, 0);
    v3d_wr(V3D_CORE0_OFFSET + V3D_CTL_L2TFLEND, 0xffffffffu);
    v3d_wr(V3D_CORE0_OFFSET + V3D_CTL_L2TCACTL,
           V3D_L2TCACTL_L2TFLS | (V3D_L2TCACTL_FLM_FLUSH << V3D_L2TCACTL_FLM_SHIFT));
    v3d_wait(v3d_l2t_flush_done, 100);
}
/* Pre-kick invalidate, "outside in" like Linux v3d_invalidate_caches: flush the L2T
 * FIRST, then invalidate the slices, so a nearby line cannot be pulled back into an
 * inner cache leaving stale data. (The non-cacheable pool already guarantees CPU->RAM
 * for the CLs/PT; this drops the GPU's own stale lines from a prior job.) */
static void v3d_invalidate_gpu_caches(void) {
    v3d_l2t_flush();
    v3d_wr(V3D_CORE0_OFFSET + V3D_CTL_SLCACTL, V3D_SLCACTL_ALL_INVALIDATE);
}

/* Poll a CLE frame counter (BFC/RFC) for an increment. 1 = advanced, 0 = deadline,
 * -3 = MMU fault latched on the hub. Deadline-bounded on cntpct, never iterations. */
static int v3d_poll_frame(uint32_t cnt_off, uint32_t base, int ms) {
    for (uint64_t dl = mono_deadline_ms(ms); mono_before(dl); ) {
        arch_dmb();
        if (v3d_rd(V3D_HUB_INT_STS) &
            (V3D_HUB_INT_MMU_PTI | V3D_HUB_INT_MMU_WRV | V3D_HUB_INT_MMU_CAP)) return -3;
        if ((v3d_rd(cnt_off) & 0xff) != (base & 0xff)) return 1;
    }
    arch_dmb();
    return ((v3d_rd(cnt_off) & 0xff) != (base & 0xff)) ? 1 : 0;
}

/* Poll BFC for bin-done, supplying overflow memory on OUTOMEM (W1C-clear the bit, else
 * the latched status re-triggers the supply every pass). A SECOND OUTOMEM in one frame
 * aborts -- the single reserve must not be handed out twice. 1 = done, 0 = deadline,
 * -2 = double OUTOMEM, -3 = MMU fault. */
static int v3d_poll_bin(uint32_t bfc0, uint32_t ovf_va, uint32_t ovf_size, int *oom_out) {
    int oom = 0;
    for (uint64_t dl = mono_deadline_ms(100); mono_before(dl); ) {
        arch_dmb();
        if (v3d_rd(V3D_HUB_INT_STS) &
            (V3D_HUB_INT_MMU_PTI | V3D_HUB_INT_MMU_WRV | V3D_HUB_INT_MMU_CAP)) {
            *oom_out = oom; return -3;
        }
        if (v3d_rd(V3D_CORE0_OFFSET + V3D_CTL_INT_STS) & V3D_CTL_INT_OUTOMEM) {
            if (oom >= 1) { *oom_out = oom; return -2; }
            v3d_wr(V3D_CORE0_OFFSET + V3D_PTB_BPOA, ovf_va);
            v3d_wr(V3D_CORE0_OFFSET + V3D_PTB_BPOS, ovf_size);
            v3d_wr(V3D_CORE0_OFFSET + V3D_CTL_INT_CLR, V3D_CTL_INT_OUTOMEM);
            arch_dsb();
            oom++;
        }
        if ((v3d_rd(V3D_CORE0_OFFSET + V3D_CLE_BFC) & 0xff) != (bfc0 & 0xff)) {
            *oom_out = oom; return 1;
        }
    }
    *oom_out = oom;
    return 0;
}

/* Capture the CLE/MMU postmortem into v3d_test, then full-reset the GPU (assert
 * V3DRSTN, re-run the power sequence + MMU init) so the next job starts clean. The
 * single most useful number is CT*CA minus QBA -- the byte offset of the hung packet. */
static void v3d_dump_and_reset(uint32_t ct0qba, uint32_t ct1qba) {
    v3d_test.ct0cs = v3d_rd(V3D_CORE0_OFFSET + V3D_CLE_CT0CS);
    v3d_test.ct1cs = v3d_rd(V3D_CORE0_OFFSET + V3D_CLE_CT1CS);
    v3d_test.ct0ca = v3d_rd(V3D_CORE0_OFFSET + V3D_CLE_CT0CA);
    v3d_test.ct1ca = v3d_rd(V3D_CORE0_OFFSET + V3D_CLE_CT1CA);
    v3d_test.ct0ra = v3d_rd(V3D_CORE0_OFFSET + V3D_CLE_CT0RA);
    v3d_test.ct1ra = v3d_rd(V3D_CORE0_OFFSET + V3D_CLE_CT1RA);
    v3d_test.ct0qba = ct0qba; v3d_test.ct1qba = ct1qba;
    v3d_test.hub_ist  = v3d_rd(V3D_HUB_INT_STS);
    v3d_test.core_ist = v3d_rd(V3D_CORE0_OFFSET + V3D_CTL_INT_STS);
    v3d_test.vio_addr = v3d_rd(V3D_MMU_VIO_ADDR);
    v3d_test.vio_id   = v3d_rd(V3D_MMU_VIO_ID);
    v3d_test.dbg      = v3d_rd(V3D_MMU_DEBUG_INFO);
    v3d_test.pt_base  = v3d_rd(V3D_MMU_PT_PA_BASE);
    AIOS_LOG_WARN("V3D job failed -- dump captured, resetting GPU");

    uint32_t pm = dev_pm_vaddr[V3D_PM_GRAFX / 4];
    dev_pm_vaddr[V3D_PM_GRAFX / 4] = V3D_PM_PASSWORD | (pm & ~V3D_PM_V3DRSTN);  /* assert reset */
    arch_dsb();
    v3d_udelay(10);
    char tmp[256];
    v3d_power_seq(0, tmp, sizeof(tmp));   /* deassert + ASB unstop + IDENT (sets v3d_ok_state) */
    v3d_mmu_inited = 0;
    v3d_mmu_init();
    v3d_resets++;
}

/* Run the bin + render doorbell brackets for already-built CLs (clear or triangle).
 * Bracket = pool/PT writes -> dsb -> CT*Q* setup -> dsb -> CT*QEA (kick = write end
 * addr) -> dsb (DESIGN sec 5). Every wait deadline-bounded. On any failure dumps +
 * resets the GPU and returns the negative status; *res carries the counter deltas.
 * Shared submit core so the triangle reuses the HW-proven Phase 2 path verbatim. */
static int v3d_run_cls(struct v3d_bo *ts, struct v3d_bo *ta, struct v3d_bo *ovf,
                       struct v3d_bo *bcl, int bn, struct v3d_bo *rcl, int rn,
                       struct v3d_clear_result *res) {
    /* zero the MMU scratch word so the post-job redirect check (scratch0 != 0 => a
     * store landed on the ILLEGAL_ADDR sink) reflects THIS job only. Non-cacheable. */
    *(volatile uint32_t *)(v3d_pool_va + V3D_SCRATCH_OFF) = 0;
    arch_dsb();   /* CLs live in the non-cacheable pool; order them before the doorbell */

    v3d_invalidate_gpu_caches();

    /* capture frame counters BEFORE the kick (capture-before-notify, v0.4.147). */
    res->bfc0 = v3d_rd(V3D_CORE0_OFFSET + V3D_CLE_BFC);
    res->rfc0 = v3d_rd(V3D_CORE0_OFFSET + V3D_CLE_RFC);
    v3d_wr(V3D_CORE0_OFFSET + V3D_CTL_INT_CLR, 0xffffffffu);   /* stale OUTOMEM/done */
    v3d_wr(V3D_HUB_INT_CLR, V3D_HUB_INT_MMU_PTI | V3D_HUB_INT_MMU_WRV | V3D_HUB_INT_MMU_CAP);
    arch_dsb();

    /* ---- bin bracket (CT0) ---- */
    v3d_wr(V3D_CORE0_OFFSET + V3D_PTB_BPOS, 0);              /* clear stale overflow size */
    v3d_wr(V3D_CORE0_OFFSET + V3D_CLE_CT0QMA, ta->gpu_va);  /* tile-alloc base */
    v3d_wr(V3D_CORE0_OFFSET + V3D_CLE_CT0QMS, ta->size);    /* tile-alloc size */
    v3d_wr(V3D_CORE0_OFFSET + V3D_CLE_CT0QTS, ts->gpu_va | V3D_CLE_CT0QTS_ENABLE);
    arch_dsb();
    v3d_wr(V3D_CORE0_OFFSET + V3D_CLE_CT0QBA, bcl->gpu_va);
    arch_dsb();
    uint64_t t0 = mono_ticks();
    v3d_wr(V3D_CORE0_OFFSET + V3D_CLE_CT0QEA, bcl->gpu_va + (uint32_t)bn);   /* KICK */
    arch_dsb();
    int br = v3d_poll_bin(res->bfc0, ovf->gpu_va, ovf->size, &res->oom);
    res->bin_us = mono_us_since(t0);
    res->bfc1 = v3d_rd(V3D_CORE0_OFFSET + V3D_CLE_BFC);
    if (br != 1) {
        v3d_dump_and_reset(bcl->gpu_va, 0);
        return (br == -2) ? -2 : (br == -3) ? -3 : -1;
    }

    /* ---- render bracket (CT1) ---- */
    arch_dsb();
    v3d_wr(V3D_CORE0_OFFSET + V3D_CLE_CT1QBA, rcl->gpu_va);
    arch_dsb();
    uint64_t t1 = mono_ticks();
    v3d_wr(V3D_CORE0_OFFSET + V3D_CLE_CT1QEA, rcl->gpu_va + (uint32_t)rn);   /* KICK */
    arch_dsb();
    int rr = v3d_poll_frame(V3D_CORE0_OFFSET + V3D_CLE_RFC, res->rfc0, 250);
    res->rend_us = mono_us_since(t1);
    res->rfc1 = v3d_rd(V3D_CORE0_OFFSET + V3D_CLE_RFC);
    if (rr != 1) {
        v3d_dump_and_reset(bcl->gpu_va, rcl->gpu_va);
        return (rr == -3) ? -3 : -4;
    }
    /* render done: flush the GPU L2T so the FB stores land in RAM for the display
     * controller (scanout reads RAM) and the CPU pixel probe. Without this RFC can
     * advance with the pixels still in the GPU cache -- "RFC++ but no orange". */
    v3d_l2t_flush();
    return 0;
}

/* Build the bin+render clear CLs into pool BOs and run the empty-bin + render pair. */
static int v3d_submit_frame(uint32_t color, struct v3d_clear_result *res) {
    res->oom = 0; res->bin_us = res->rend_us = 0;
    res->bfc0 = res->bfc1 = res->rfc0 = res->rfc1 = 0;
    if (v3d_mmu_init()) return -5;
    v3d_ensure_clock();

    v3d_bo_reset_frame();
    struct v3d_bo ts, ta, ovf, bcl, rcl;
    if (v3d_bo_alloc(&ts, 48u * 1024, 4096) || v3d_bo_alloc(&ta, 40u * 1024, 4096) ||
        v3d_bo_alloc(&ovf, 256u * 1024, 4096) ||
        v3d_bo_alloc(&bcl, 256, 128) || v3d_bo_alloc(&rcl, 4096, 128))
        return -5;

    struct v3d_clear_params cp = {
        .width = (uint16_t)gpu_width, .height = (uint16_t)gpu_height,
        .clear_color = color, .fb_va = V3D_VA_FB,
        .fb_stride = gpu_width * 4u, .tile_alloc_va = ta.gpu_va,
        .rb_swap = 0,   /* AIOS FB is BGR (pixel-ord 0): store verbatim, no R/B swap */
    };
    int bn = v3d_build_bin_cl(bcl.cpu, (int)bcl.size, &cp);
    int rn = v3d_build_render_cl(rcl.cpu, (int)rcl.size, rcl.gpu_va, &cp);
    if (bn < 0 || rn < 0) return -5;
    return v3d_run_cls(&ts, &ta, &ovf, &bcl, bn, &rcl, rn, res);
}

/* ---- Phase 3: build the triangle BOs (shaders/records/uniforms/geometry/CLs) and
 * submit via the shared core. Reuses v3d_run_cls verbatim. ---- */
#define V3D_TRI_CLEAR_COLOR  0xff101010u   /* dark gray, so the rainbow stands out */

static void v3d_load_words(uint8_t *dst, const uint64_t *src, unsigned nwords) {
    volatile uint32_t *d = (volatile uint32_t *)dst;
    for (unsigned i = 0; i < nwords; i++) { d[2 * i] = (uint32_t)src[i]; d[2 * i + 1] = (uint32_t)(src[i] >> 32); }
}

static int v3d_submit_triangle(struct v3d_clear_result *res) {
    res->oom = 0; res->bin_us = res->rend_us = 0;
    res->bfc0 = res->bfc1 = res->rfc0 = res->rfc1 = 0;
    if (v3d_mmu_init()) return -5;
    v3d_ensure_clock();
    uint32_t w = gpu_width, h = gpu_height;

    v3d_bo_reset_frame();
    struct v3d_bo ts, ta, ovf, zb, shc, dattr, unif, recat, posd, cold, tlist, bcl, rcl;
    if (v3d_bo_alloc(&ts, 48u * 1024, 4096) || v3d_bo_alloc(&ta, 40u * 1024, 4096) ||
        v3d_bo_alloc(&ovf, 256u * 1024, 4096) ||
        v3d_bo_alloc(&zb, 2u * 1024 * 1024, 4096) ||   /* D16 depth (we store but never read) */
        v3d_bo_alloc(&shc, 512, 8) ||                  /* frag+vtx+coord shaders (8-aligned) */
        v3d_bo_alloc(&dattr, 256, 16) ||               /* default attribute values */
        v3d_bo_alloc(&unif, 32, 16) ||                 /* viewport uniforms (5 + 3 floats) */
        v3d_bo_alloc(&recat, 36 + 16 + 16, 16) ||      /* GL shader state record + 2 attr records (contiguous!) */
        v3d_bo_alloc(&posd, 36, 16) || v3d_bo_alloc(&cold, 16, 16) ||  /* pos + color vertex data */
        v3d_bo_alloc(&tlist, 64, 16) ||                /* generic tile list */
        v3d_bo_alloc(&bcl, 256, 128) || v3d_bo_alloc(&rcl, 4096, 128))
        return -5;
    /* the GPU stores a D16 (2 bytes/px) Z buffer to zb -- fail loudly if a future FB
     * cap ever makes the frame bigger than the fixed Z BO (silent overrun would
     * corrupt the adjacent CL/record BOs in the pool). 1024x768 D16 = 1.5MB < 2MB. */
    if ((uint64_t)w * h * 2u > zb.size) { AIOS_LOG_ERROR("V3D: Z buffer too small for FB"); return -5; }

    /* shaders consecutive, 8-aligned: frag(12) then vtx(25) then coord(18) x u64 */
    uint32_t frag_code = shc.gpu_va;
    uint32_t vtx_code  = shc.gpu_va + (uint32_t)(V3D_FRAG_SHADER_WORDS * 8);
    uint32_t coord_code = vtx_code + (uint32_t)(V3D_VTX_SHADER_WORDS * 8);
    v3d_load_words(shc.cpu, v3d_frag_shader, V3D_FRAG_SHADER_WORDS);
    v3d_load_words(shc.cpu + V3D_FRAG_SHADER_WORDS * 8, v3d_vtx_shader, V3D_VTX_SHADER_WORDS);
    v3d_load_words(shc.cpu + (V3D_FRAG_SHADER_WORDS + V3D_VTX_SHADER_WORDS) * 8,
                   v3d_coord_shader, V3D_COORD_SHADER_WORDS);

    /* default attribute values: 16 x vec4(0,0,0,1) */
    { volatile uint32_t *d = (volatile uint32_t *)dattr.cpu;
      for (int i = 0; i < 16; i++) { d[4*i] = 0; d[4*i+1] = 0; d[4*i+2] = 0; d[4*i+3] = 0x3f800000u; } }

    /* viewport uniforms: frag/vtx share [1.0, (w/2)*256, (h/2)*-256, 0.5, 0.5];
     * coord = [1.0, (w/2)*256, (h/2)*-256]. */
    { volatile uint32_t *u = (volatile uint32_t *)unif.cpu;
      uint32_t hw = v3d_i2f((int32_t)((w / 2) * 256)), hh = v3d_i2f(-(int32_t)((h / 2) * 256));
      u[0] = 0x3f800000u; u[1] = hw; u[2] = hh; u[3] = 0x3f000000u; u[4] = 0x3f000000u;
      u[5] = 0x3f800000u; u[6] = hw; u[7] = hh; }
    uint32_t frag_unif = unif.gpu_va, vtx_unif = unif.gpu_va, coord_unif = unif.gpu_va + 20u;

    /* vertex data */
    { volatile uint32_t *p = (volatile uint32_t *)posd.cpu;
      const uint32_t *src = (const uint32_t *)v3d_tri_pos;
      for (int i = 0; i < 9; i++) p[i] = src[i]; }
    { volatile uint8_t *c = (volatile uint8_t *)cold.cpu;
      for (int i = 0; i < 12; i++) c[i] = v3d_tri_color[i]; }

    /* GL Shader State Record + the two attribute records, CONTIGUOUS (the HW reads
     * nattr attribute records immediately after the 36-byte record). */
    struct v3d_shader_record_params srp = {
        .default_attr_va = dattr.gpu_va,
        .frag_code_va = frag_code, .frag_unif_va = frag_unif,
        .vtx_code_va = vtx_code, .vtx_unif_va = vtx_unif,
        .coord_code_va = coord_code, .coord_unif_va = coord_unif,
        .num_fs_varyings = 4,
    };
    if (v3d_build_shader_record(recat.cpu, 36, &srp) < 0) return -5;
    struct v3d_attr_params pos = { .address = posd.gpu_va, .vec_size = 3, .type = 2, .normalized = 0,
        .num_read_vtx = 3, .num_read_coord = 3, .stride = 12, .max_index = 0xFFFFFFu };
    struct v3d_attr_params col = { .address = cold.gpu_va, .vec_size = 0, .type = 4, .normalized = 1,
        .num_read_vtx = 4, .num_read_coord = 0, .stride = 4, .max_index = 0xFFFFFFu };
    if (v3d_build_attr_record(recat.cpu + 36, 16, &pos) < 0) return -5;
    if (v3d_build_attr_record(recat.cpu + 52, 16, &col) < 0) return -5;

    struct v3d_tri_params tp = {
        .width = (uint16_t)w, .height = (uint16_t)h, .clear_color = V3D_TRI_CLEAR_COLOR,
        .rb_swap = 1,   /* RGBA shader colors -> AIOS BGR FB needs the R/B swap (verify on HW) */
        .fb_va = V3D_VA_FB, .fb_stride = w * 4u, .z_va = zb.gpu_va,
        .tile_alloc_va = ta.gpu_va, .shader_record_va = recat.gpu_va, .nattr = 2,
        .tile_list_va = tlist.gpu_va, .vertex_count = 3,
    };
    if (v3d_build_triangle_tile_list(tlist.cpu, (int)tlist.size, &tp) < 0) return -5;
    int bn = v3d_build_triangle_bin_cl(bcl.cpu, (int)bcl.size, &tp);
    int rn = v3d_build_triangle_render_cl(rcl.cpu, (int)rcl.size, &tp);
    if (bn < 0 || rn < 0) return -5;
    return v3d_run_cls(&ts, &ta, &ovf, &bcl, bn, &rcl, rn, res);
}

/* Probe the live FB after a clear: CleanInvalidate the page holding the center pixel,
 * read it, compare to the requested FB-order value. Also sample the MMU scratch first
 * word -- non-zero means the GPU silently redirected stores there (a wrong RT VA,
 * design risk R3): RFC can advance with no visible pixels, so never trust RFC alone. */
static void v3d_pixel_probe(uint32_t color) {
    uint32_t idx  = (gpu_height / 2) * gpu_width + (gpu_width / 2);
    uint32_t page = (uint32_t)(((uint64_t)idx * 4) >> 12);
    gpu_fb_invalidate_page(page);
    arch_dmb();
    v3d_test.pixel      = gpu_fb[idx];
    v3d_test.pixel_pass = (v3d_test.pixel == color);
    v3d_test.scratch0   = v3d_pool_va
        ? *(volatile uint32_t *)(v3d_pool_va + V3D_SCRATCH_OFF) : 0;
}

/* The full kick, display_server thread only: take FB ownership (clean every line to
 * RAM, then suspend the console so no dirty CPU line writes back over GPU pixels),
 * submit the clear, probe the center pixel. On failure recover the console (the GPU
 * was reset). Fills v3d_test. Returns 0 on PASS. */
int v3d_clear_and_probe(uint32_t color) {
    v3d_tests_run++;
    v3d_test.valid = 1;
    v3d_test.color = color;
    v3d_test.pixel = 0; v3d_test.pixel_pass = 0; v3d_test.scratch0 = 0;
    if (!v3d_ok_state) { char id[160]; v3d_report_ident(id, sizeof(id), 0, 0); }
    if (!v3d_present || !v3d_ok_state || !gpu_available || !gpu_fb_pa || !v3d_pool_va) {
        v3d_test.status = -5;
        return -5;
    }
    gpu_fb_flush_all();          /* every FB line clean -> evictions cannot clobber pixels */
    fb_console_set_suspend(1);   /* gate the console for the duration of GPU ownership   */

    struct v3d_clear_result res;
    int st = v3d_submit_frame(color, &res);
    v3d_test.status = st;
    v3d_test.bfc0 = res.bfc0; v3d_test.bfc1 = res.bfc1;
    v3d_test.rfc0 = res.rfc0; v3d_test.rfc1 = res.rfc1;
    v3d_test.bin_us = res.bin_us; v3d_test.rend_us = res.rend_us;
    v3d_test.oom = res.oom;

    if (st == 0) {
        v3d_test.is_tri = 0;
        v3d_pixel_probe(color);                 /* orange should be visible now */
    } else {
        fb_console_set_suspend(0);              /* GPU was reset -- restore the console */
        fb_console_clear();
    }
    return st;
}

/* The full triangle kick, display_server thread only (mirrors v3d_clear_and_probe):
 * take FB ownership, submit the GL-draw, probe. PASS = center pixel differs from the
 * clear color (the triangle covers center) with no scratch redirect; the corners are
 * sampled as a visual aid. The rainbow triangle on the monitor is the real proof. */
int v3d_triangle_and_probe(void) {
    v3d_tests_run++;
    v3d_test.valid = 1;
    v3d_test.color = V3D_TRI_CLEAR_COLOR;
    v3d_test.pixel = 0; v3d_test.pixel_pass = 0; v3d_test.scratch0 = 0;
    v3d_test.corner_tl = 0; v3d_test.corner_br = 0; v3d_test.is_tri = 1;
    if (!v3d_ok_state) { char id[160]; v3d_report_ident(id, sizeof(id), 0, 0); }
    if (!v3d_present || !v3d_ok_state || !gpu_available || !gpu_fb_pa || !v3d_pool_va) {
        v3d_test.status = -5;
        return -5;
    }
    gpu_fb_flush_all();
    fb_console_set_suspend(1);

    struct v3d_clear_result res;
    int st = v3d_submit_triangle(&res);
    v3d_test.status = st;
    v3d_test.bfc0 = res.bfc0; v3d_test.bfc1 = res.bfc1;
    v3d_test.rfc0 = res.rfc0; v3d_test.rfc1 = res.rfc1;
    v3d_test.bin_us = res.bin_us; v3d_test.rend_us = res.rend_us;
    v3d_test.oom = res.oom;

    if (st == 0) {
        /* center (inside the triangle) + two corners (outside near the top). Each
         * sample CleanInvalidates the page it actually reads (the FB is cacheable). */
        uint32_t cidx  = (gpu_height / 2) * gpu_width + (gpu_width / 2);
        uint32_t tlidx = 10 * gpu_width + 10;
        uint32_t bridx = (gpu_height - 1) * gpu_width + (gpu_width - 1);
        gpu_fb_invalidate_page((uint32_t)(((uint64_t)cidx  * 4) >> 12));
        gpu_fb_invalidate_page((uint32_t)(((uint64_t)tlidx * 4) >> 12));
        gpu_fb_invalidate_page((uint32_t)(((uint64_t)bridx * 4) >> 12));
        arch_dmb();
        v3d_test.pixel     = gpu_fb[cidx];
        v3d_test.corner_tl = gpu_fb[tlidx];
        v3d_test.corner_br = gpu_fb[bridx];
        v3d_test.scratch0  = v3d_pool_va ? *(volatile uint32_t *)(v3d_pool_va + V3D_SCRATCH_OFF) : 0;
        v3d_test.pixel_pass = (v3d_test.pixel != V3D_TRI_CLEAR_COLOR);
    } else {
        fb_console_set_suspend(0);
        fb_console_clear();
    }
    return st;
}
#else  /* !PLAT_RPI4 -- never reached (has_v3d=0); stubs keep the dispatcher common */
int v3d_clear_and_probe(uint32_t color) { (void)color; return -5; }
int v3d_triangle_and_probe(void) { return -5; }
static int v3d_mmu_verb(char *buf, int bufsize) {
    return snprintf(buf, bufsize, "v3d.mmu: unavailable (not RPi4)\n");
}
static int v3d_fault_probe(char *buf, int bufsize) {
    return snprintf(buf, bufsize, "v3d.fault: unavailable (not RPi4)\n");
}
static int v3d_cl_probe(char *buf, int bufsize) {
    return snprintf(buf, bufsize, "v3d.cl: unavailable (not RPi4)\n");
}
#endif /* PLAT_RPI4 */

/* ---- Phase 2 trigger glue (common: dispatcher + display_server both call in) ---- */

/* Format the last clear-job result (the .test PASS/FAIL line + dump on failure). */
static int v3d_format_test_result(char *buf, int bufsize) {
    if (!v3d_test.valid)
        return snprintf(buf, bufsize, "v3d.test: no result yet -- run cat /proc/v3d.test\n");
    const char *st = (v3d_test.status ==  0) ? "OK"
                   : (v3d_test.status == -1) ? "BIN_TIMEOUT"
                   : (v3d_test.status == -2) ? "DOUBLE_OUTOMEM"
                   : (v3d_test.status == -3) ? "MMU_FAULT"
                   : (v3d_test.status == -4) ? "RENDER_TIMEOUT"
                   :                           "NO_GPU";
    int w = snprintf(buf, bufsize,
        "v3d.%s: clear=0x%08x status=%s  bfc %u->%u rfc %u->%u  bin=%lluus rend=%lluus oom=%d\n",
        v3d_test.is_tri ? "tri" : "test", v3d_test.color, st,
        v3d_test.bfc0 & 0xff, v3d_test.bfc1 & 0xff, v3d_test.rfc0 & 0xff, v3d_test.rfc1 & 0xff,
        (unsigned long long)v3d_test.bin_us, (unsigned long long)v3d_test.rend_us, v3d_test.oom);
    if (v3d_test.status == 0 && v3d_test.is_tri)
        w += snprintf(buf + w, bufsize - w,
            "  center[%u,%u]=0x%08x (!= clear 0x%08x) corners tl=0x%08x br=0x%08x scratch0=0x%08x -- %s\n",
            gpu_width / 2, gpu_height / 2, v3d_test.pixel, v3d_test.color,
            v3d_test.corner_tl, v3d_test.corner_br, v3d_test.scratch0,
            (v3d_test.pixel_pass && v3d_test.scratch0 == 0) ? "PASS" : "FAIL");
    else if (v3d_test.status == 0)
        w += snprintf(buf + w, bufsize - w,
            "  pixel[%u,%u]=0x%08x expect 0x%08x scratch0=0x%08x -- %s\n",
            gpu_width / 2, gpu_height / 2, v3d_test.pixel, v3d_test.color, v3d_test.scratch0,
            (v3d_test.pixel_pass && v3d_test.scratch0 == 0) ? "PASS" : "FAIL");
    else
        w += snprintf(buf + w, bufsize - w,
            "  CT0CA-QBA=0x%x CT1CA-QBA=0x%x CT0CS=0x%08x CT1CS=0x%08x\n"
            "  hub_int=0x%08x core_int=0x%08x VIO_ADDR=0x%08x VIO_ID=0x%08x DEBUG=0x%08x PT_BASE=0x%08x resets=%u\n",
            v3d_test.ct0ca - v3d_test.ct0qba, v3d_test.ct1ca - v3d_test.ct1qba,
            v3d_test.ct0cs, v3d_test.ct1cs, v3d_test.hub_ist, v3d_test.core_ist,
            v3d_test.vio_addr, v3d_test.vio_id, v3d_test.dbg, v3d_test.pt_base, v3d_resets);
    return w;
}

/* Called on the display_server thread when it wakes on its bound notification: run a
 * pending /proc/v3d.{test,tri} request (kind 1=clear, 2=triangle). No-op if none. */
void v3d_service_display_request(void) {
    uint32_t kind = g_v3d_req;
    if (!kind) return;
    g_v3d_req = 0;
    if (kind == 2) v3d_triangle_and_probe();
    else           v3d_clear_and_probe(g_v3d_req_color);   /* QEMU stubs -> -5 */
    arch_dsb();
    v3d_test_seq++;                          /* unblock the poller */
}

/* Shared poster for the request-flag verbs (fs thread): post kind + wake
 * display_server via the bound notification, yield-poll for completion. NO seL4_Call
 * (a nested Call from the fs thread mid procfs-read clobbers the reply cap on
 * non-MCS -- see feedback_sel4_nested_call); the flag + Signal keeps it intact. */
static int v3d_request_post(uint32_t kind, uint32_t color, const char *label,
                            char *buf, int bufsize) {
    g_v3d_req_color = color;
    uint32_t seq0 = v3d_test_seq;
    arch_dsb();
    g_v3d_req = kind;
    arch_dsb();
    if (disp_wake_ntfn_cap) seL4_Signal(disp_wake_ntfn_cap);
    for (uint64_t dl = mono_deadline_ms(2000); mono_before(dl); ) {
        if (v3d_test_seq != seq0) break;
        seL4_Yield();                /* let display_server (same core) run the job */
    }
    if (v3d_test_seq == seq0)
        return snprintf(buf, bufsize,
            "v3d.%s: queued, display_server did not finish in 2s -- "
            "cat /proc/v3d for the result\n", label);
    return v3d_format_test_result(buf, bufsize);
}

/* /proc/v3d.test = canned-orange clear; /proc/v3d.tri = rainbow triangle. */
static int v3d_test_post(char *buf, int bufsize) {
    return v3d_request_post(1, 0xFFFF8000u, "test", buf, bufsize);
}
static int v3d_tri_post(char *buf, int bufsize) {
    return v3d_request_post(2, 0, "tri", buf, bufsize);
}

/* ---- boot init: claims + IRQ bind + tag pin; ZERO V3D MMIO ---- */
void v3d_init(void) {
    if (!hw_info.has_v3d) {
        AIOS_LOG_INFO("V3D: not present (has_v3d=0)");
        return;
    }
    /* GENET rule: the MMIO must be pre-mapped by prealloc_rpi4_devices (the
     * ascending-paddr watermark owns the order); never self-map here. */
    if (!dev_v3d_vaddr || !dev_v3d_asb_vaddr) {
        printf("[v3d] ERROR: MMIO not pre-mapped (hub=%p asb=%p) -- disabled; "
               "check the prealloc boot log\n", (void *)dev_v3d_vaddr, (void *)dev_v3d_asb_vaddr);
        AIOS_LOG_ERROR("V3D: MMIO not pre-mapped -- disabled");
        return;
    }
    v3d_base = dev_v3d_vaddr;
    v3d_asb  = dev_v3d_asb_vaddr;
    v3d_present = 1;

    v3d_bind_irq();          /* IRQ 106, fully masked, single-threaded boot context */
    v3d_mbox_init();         /* pin the tag buffer NOW (watermark order); no mbox traffic */

    printf("[v3d] init OK: hub/core@0x%lx (%p) asb@0xFEC11000 (%p) irq=%d mask=all tag=%s -- "
           "ZERO V3D MMIO; power deferred to /proc/v3d.power\n",
           (unsigned long)hw_info.v3d_paddr, (void *)v3d_base, (void *)v3d_asb,
           v3d_irq_num,
#ifdef PLAT_RPI4
           v3d_mbox_ready ? "pinned@0x3A002000" : "unavailable"
#else
           "n/a"
#endif
           );
    AIOS_LOG_INFO("V3D Phase 0 init OK (power lazy)");
}

/* ---- cat /proc/v3d : one-screen summary (all reads are benign pre-power) ---- */
static int v3d_summary(char *buf, int bufsize) {
    v3d_sample_irq();
    char idln[160];
    v3d_report_ident(idln, sizeof(idln), 0, 0);   /* pure reads; updates v3d_ok_state */
    int w = 0;
    w += snprintf(buf + w, bufsize - w, "v3d: hub/core@0x%lx asb@0xFEC11000 irq=%d v3d_ok=%d\n",
                  (unsigned long)hw_info.v3d_paddr, v3d_irq_num, v3d_ok_state);
    w += snprintf(buf + w, bufsize - w, "%s", idln);
    w += snprintf(buf + w, bufsize - w,
                  "hub_int_sts=0x%08x core_int_sts=0x%08x irq_count=%u\n",
                  v3d_rd(V3D_HUB_INT_STS), v3d_rd(V3D_CORE0_OFFSET + V3D_CTL_INT_STS), v3d_irqs);
    w += snprintf(buf + w, bufsize - w,
                  "power_pokes=%u asb_ack: m=%lluus%s s=%lluus%s\n", v3d_power_count,
                  (unsigned long long)v3d_asb_m_us, v3d_asb_m_to ? "(TO)" : "",
                  (unsigned long long)v3d_asb_s_us, v3d_asb_s_to ? "(TO)" : "");
    w += snprintf(buf + w, bufsize - w,
                  "pool=%s mmu=%s mmu_faults_seen=%u last_vio_addr=0x%08x vio_id=0x%08x\n",
                  v3d_pool_va ? "8MB" : "none", v3d_mmu_inited ? "ON" : "off",
                  v3d_mmu_faults_seen, v3d_fault_vio_addr, v3d_fault_vio_id);
    w += snprintf(buf + w, bufsize - w, "tests_run=%u resets=%u\n",
                  v3d_tests_run, v3d_resets);
    if (v3d_test.valid) w += v3d_format_test_result(buf + w, bufsize - w);
    return w;
}

/* ---- /proc/v3d dispatcher (bring-up regime: direct on the fs thread) ---- */
int v3d_diag_cmd(const char *args, char *buf, int bufsize) {
    if (!v3d_present)
        return snprintf(buf, bufsize, "v3d: not present\n");

    if (args[0] == '.') {
        const char *p = args + 1;
        if (v3d_pfx(p, "power") && (p[5] == 0 || p[5] == '.')) {
            int step = (p[5] == '.') ? (int)v3d_dec(p + 6) : 0;
            return v3d_power_seq(step, buf, bufsize);
        }
        if (v3d_pfx(p, "clock") && (p[5] == 0 || p[5] == '.')) {
            if (p[5] == '.') return v3d_clock_verb(1, v3d_dec(p + 6), buf, bufsize);
            return v3d_clock_verb(0, 0, buf, bufsize);
        }
        if (v3d_pfx(p, "fault") && p[5] == 0) return v3d_fault_probe(buf, bufsize);
        if (v3d_pfx(p, "mmu")   && p[3] == 0) return v3d_mmu_verb(buf, bufsize);
        if (v3d_pfx(p, "cl")    && p[2] == 0) return v3d_cl_probe(buf, bufsize);
        if (v3d_pfx(p, "test")  && p[4] == 0) return v3d_test_post(buf, bufsize);
        if (v3d_pfx(p, "tri")   && p[3] == 0) return v3d_tri_post(buf, bufsize);
        if (p[0] == 'r' && p[1] == '.') {
            uint32_t off = v3d_hex(p + 2) & 0x3ffc;
            return snprintf(buf, bufsize, "v3d hub[0x%03x] = 0x%08x\n", off, v3d_rd(off));
        }
        if (p[0] == 'c' && p[1] == '.') {
            uint32_t off = v3d_hex(p + 2) & 0x3ffc;
            return snprintf(buf, bufsize, "v3d core0[0x%03x] = 0x%08x\n", off,
                            v3d_rd(V3D_CORE0_OFFSET + off));
        }
        if (p[0] == 'w' && p[1] == '.') {
            const char *q = p + 2;
            uint32_t off = v3d_hex(q) & 0x3ffc;
            while (*q && *q != '.') q++;
            if (*q != '.') return snprintf(buf, bufsize, "v3d: usage .w.<hexoff>.<hexval>\n");
            uint32_t val = v3d_hex(q + 1);
            v3d_wr(off, val);
            return snprintf(buf, bufsize, "v3d hub[0x%03x] <- 0x%08x (readback 0x%08x)\n",
                            off, val, v3d_rd(off));
        }
        return snprintf(buf, bufsize,
            "v3d: verbs: (none) .power[.N] .clock[.MHz] .mmu .fault .cl .test .tri "
            ".r.<off> .c.<off> .w.<off>.<val>\n");
    }
    return v3d_summary(buf, bufsize);
}

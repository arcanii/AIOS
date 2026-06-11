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
#define VC_TAG_GET_CLOCK_RATE  0x00030002u
#define VC_TAG_SET_CLOCK_RATE  0x00038002u
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
static int v3d_vc_tag(uint32_t tag, uint32_t *vals, int nwords) {
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

#else  /* !PLAT_RPI4 -- QEMU has no V3D model; these are never reached (has_v3d=0) */

static void v3d_mbox_init(void) {}
static int  v3d_power_seq(int only_step, char *buf, int bufsize) {
    (void)only_step;
    return snprintf(buf, bufsize, "v3d: power sequence unavailable (not RPi4)\n");
}
static int  v3d_clock_verb(int set, uint32_t mhz, char *buf, int bufsize) {
    (void)set; (void)mhz;
    return snprintf(buf, bufsize, "v3d: clock unavailable (not RPi4)\n");
}

#endif /* PLAT_RPI4 */

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
            "v3d: verbs: (none) .power[.N] .clock[.MHz] .r.<off> .c.<off> .w.<off>.<val>\n");
    }
    return v3d_summary(buf, bufsize);
}

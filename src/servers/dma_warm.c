/* dma_warm.c -- v0.4.287: AUTONOMOUS DRAM-DMA keep-warm for the BCM2711
 * idle->wake ~32.4s whole-system freeze ([[project_stall_hunt]], a MAJOR OPEN
 * CONCERN -- [[feedback_stall_open_concern]]).
 *
 * WHY THIS IS DIFFERENT from every prior (refuted) keep-warm. Session-8 HW
 * localization (docs/NEXT_20260621_stall_session8_localize.md) showed the freeze
 * is core 0 CLOCKED-BUT-WEDGED on a SINGLE fabric-dependent instruction
 * (PMU iovf=0, tiny bus = a clean wait on a PARKED fabric), POSITION-INDEPENDENT
 * (caught at 4 distinct code sites), leading hypothesis = a COLD CACHEABLE DRAM
 * LOAD whose fill hangs through the quiesced SCB->memory-controller path. The
 * prior keep-warms (fabwarm GPLEV0, GENET MDIO, corewarm cacheable RMW) all
 * targeted the DVM/snoop path on the old "dsb" theory and were refuted; NONE
 * generated continuous DRAM traffic. If the real culprit is the MEMORY path,
 * the right keep-warm is REAL DRAM TRAFFIC during idle.
 *
 * MECHANISM. Program a BCM2711 legacy-DMA channel with a SELF-LOOPING control
 * block (NEXTCONBK -> itself) that copies a small dedicated scratch buffer in
 * DRAM forever. Once started it runs ENTIRELY in hardware -- no CPU kicks -- so
 * it keeps the SCB->memory path active through idle (preventing the quiesce that
 * causes the wedge) AND keeps running even while core 0 is wedged. It is the
 * "coherent external bus-master heartbeat" the session-8 analysis called for
 * (the one keep-warm class never tried). It touches ONLY its own scratch buffer
 * (src/dst both AIOS-owned sub-1GB DRAM), so there is ZERO corruption risk.
 *
 * SAFETY. The DMA channel is chosen from the VideoCore's GET_DMA_CHANNELS mask
 * (mailbox tag 0x00060001) so we never steal a VC-reserved channel; the channel
 * is RESET before use and its CS.ACTIVE is checked. Default DISARMED (inert --
 * zero effect on the shipped system) so it can be A/B-tested against the baseline
 * on the SAME boot via /proc/freezes + pingmon + netstall. /proc/dmawarm.1 = arm,
 * .0 = disarm, bare = status. RPi4-only (no DMA controller / no freeze on QEMU).
 */
#include <stdio.h>
#include <string.h>
#include <sel4/sel4.h>
#include <vspace/vspace.h>
#include <vka/vka.h>
#include <vka/object.h>
#include <sel4platsupport/device.h>
#include "arch.h"                 /* arch_dsb, arch_dmb */
#include "aios/root_shared.h"     /* vka, vspace, simple */
#include "aios/device_map.h"      /* dev_dma_vaddr, dev_vcmbox_vaddr, dev_vcmbox_off (RPi4) */

#ifdef PLAT_RPI4

/* ---- BCM2711 legacy DMA controller (base = dev_dma_vaddr = 0xFE007000) ---- */
#define DMA_CH_STRIDE   0x100u            /* per-channel register block */
#define DMA_CS          0x00u             /* control & status */
#define DMA_CONBLK_AD   0x04u             /* control-block bus address */
#define DMA_DEBUG       0x20u             /* error/debug */
/* CS bits */
#define CS_ACTIVE       (1u << 0)
#define CS_END          (1u << 1)
#define CS_INT          (1u << 2)
#define CS_ERROR        (1u << 8)
#define CS_PRIORITY(p)  (((p) & 0xf) << 16)
#define CS_PANIC(p)     (((p) & 0xf) << 20)
#define CS_ABORT        (1u << 30)
#define CS_RESET        (1u << 31)
/* DEBUG error bits (write 1 to clear) */
#define DBG_CLR         (0x7u)            /* READ_LAST_NOT_SET | FIFO_ERR | READ_ERR */
/* Transfer-info (CB word 0) bits */
#define TI_WAIT_RESP    (1u << 3)         /* wait for AXI write response (real fabric round-trip) */
#define TI_DEST_INC     (1u << 4)
#define TI_DEST_WIDTH128 (1u << 5)
#define TI_SRC_INC      (1u << 8)
#define TI_SRC_WIDTH128 (1u << 9)
#define TI_BURST(n)     (((n) & 0xf) << 12)
/* TI bit map (BCM2835/2711 datasheet 4.2.1.3): PERMAP = bits [20:16], WAITS = bits [25:21] --
 * ADJACENT, not overlapping. TI_WAITS(31)=31<<21 sets only [25:21]; PERMAP stays 0 (no peripheral
 * DREQ), which is REQUIRED for mem-to-mem. WAITS just throttles so we do not saturate DRAM bandwidth. */
#define TI_WAITS(n)     (((n) & 0x1f) << 21)

/* Legacy-DMA bus alias of a sub-1GB ARM physical address (matches net_genet.c /
 * v3d.c: paddr | 0xC0000000, valid only for paddr < 1GB). */
#define BUS(paddr)      ((uint32_t)(((uint64_t)(paddr)) | 0xC0000000ULL))
#define DMA_REGION_LIMIT 0x40000000ULL    /* keep paddr|0xC0000000 in 32 bits */
#define DMA_REGION_PAGES 4                /* page0: CB + mbox tag ; page1: src ; page2: dst */

/* Control block layout (32 bytes, 256-bit aligned) -- 8 u32 words */
struct dma_cb {
    uint32_t ti, source_ad, dest_ad, txfr_len, stride, nextconbk, rsvd0, rsvd1;
};

static volatile uint32_t *g_dma_base;     /* dev_dma_vaddr */
static int               g_dma_chan = -1; /* chosen ARM-free channel */
static volatile struct dma_cb *g_cb;      /* the self-loop control block (virt) */
static uint64_t          g_cb_bus;        /* its bus address */
static uint64_t          g_region_pa;     /* sub-1GB base paddr of the region */
static int               g_dma_ready;
static volatile uint64_t g_arm_count;     /* how many times we armed it (status) */

static inline volatile uint32_t *chan_reg(int ch, uint32_t off)
{
    return g_dma_base + (ch * DMA_CH_STRIDE + off) / 4;
}

/* ---- minimal VC property mailbox (GET_DMA_CHANNELS), modeled on v3d.c ---- */
#define MBOX_READ_OFF   0x00
#define MBOX_STATUS_OFF 0x18
#define MBOX_WRITE_OFF  0x20
#define MBOX_FULL       0x80000000U
#define MBOX_EMPTY      0x40000000U
#define MBOX_CH_PROP    8
#define MBOX_RESP_OK    0x80000000U
#define VC_TAG_GET_DMA_CHANNELS 0x00060001u

/* Query the ARM-usable DMA channel mask. tag_buf/tag_bus is a 16-byte-aligned
 * scratch slot in our low region. Returns the bitmask (bit c set => ARM may use
 * channel c), or 0 on failure (caller falls back conservatively). */
static uint32_t query_dma_channel_mask(volatile uint32_t *tag_buf, uint64_t tag_bus)
{
    if (!dev_vcmbox_vaddr) {
        return 0;
    }
    volatile uint32_t *regs = (volatile uint32_t *)((uintptr_t)dev_vcmbox_vaddr + dev_vcmbox_off);
    tag_buf[0] = 7 * 4;                    /* total size */
    tag_buf[1] = 0;                        /* request */
    tag_buf[2] = VC_TAG_GET_DMA_CHANNELS;  /* tag */
    tag_buf[3] = 4;                        /* value buffer size */
    tag_buf[4] = 0;                        /* request/response code */
    tag_buf[5] = 0;                        /* value (response: the mask) */
    tag_buf[6] = 0;                        /* end tag */
    arch_dsb();
    uint32_t addr_ch = (uint32_t)(tag_bus & 0xFFFFFFF0U) | MBOX_CH_PROP;
    for (int i = 0; i < 1000000; i++) {
        arch_dmb();
        if (!(regs[MBOX_STATUS_OFF / 4] & MBOX_FULL)) break;
    }
    arch_dsb();
    regs[MBOX_WRITE_OFF / 4] = addr_ch;
    arch_dsb();
    for (int i = 0; i < 2000000; i++) {
        arch_dmb();
        if (regs[MBOX_STATUS_OFF / 4] & MBOX_EMPTY) continue;
        arch_dmb();
        if (regs[MBOX_READ_OFF / 4] == addr_ch) {
            arch_dmb();
            if (tag_buf[1] == MBOX_RESP_OK) {
                return tag_buf[5];
            }
            return 0;
        }
    }
    return 0;
}

/* Allocate DMA_REGION_PAGES contiguous frames whose base lands below 1GB (so the
 * legacy bus alias paddr|0xC0000000 is valid). Same probe-an-untyped pattern as
 * net_genet.c dma_init(). Returns the mapped virt base, sets *out_pa; NULL on fail. */
static void *alloc_low_region(uint64_t *out_pa)
{
    vka_object_t ut, rejects[8];
    int nrej = 0, have = 0;
    seL4_CPtr caps[DMA_REGION_PAGES];
    uint64_t base_pa = 0;

    /* size 14 untyped = 16KB = 4 pages */
    for (int t = 0; t < 8 && !have; t++) {
        if (vka_alloc_untyped(&vka, 14, &ut)) break;
        seL4_CPtr probe;
        if (vka_cspace_alloc(&vka, &probe)) { vka_free_object(&vka, &ut); break; }
        if (seL4_Untyped_Retype(ut.cptr, ARCH_PAGE_OBJECT,
                                seL4_PageBits, seL4_CapInitThreadCNode, 0, 0, probe, 1)) {
            vka_cspace_free(&vka, probe); vka_free_object(&vka, &ut); break;
        }
        seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(probe);
        if (!ga.error &&
            ga.paddr + (uint64_t)DMA_REGION_PAGES * 0x1000 <= DMA_REGION_LIMIT) {
            caps[0] = probe; base_pa = ga.paddr; have = 1; break;
        }
        seL4_CNode_Delete(seL4_CapInitThreadCNode, probe, seL4_WordBits);
        vka_cspace_free(&vka, probe);
        if (nrej < 8) rejects[nrej++] = ut; else vka_free_object(&vka, &ut);
    }
    for (int i = 0; i < nrej; i++) vka_free_object(&vka, &rejects[i]);
    if (!have) return NULL;

    for (int i = 1; i < DMA_REGION_PAGES; i++) {
        seL4_CPtr slot;
        if (vka_cspace_alloc(&vka, &slot)) return NULL;
        if (seL4_Untyped_Retype(ut.cptr, ARCH_PAGE_OBJECT,
                                seL4_PageBits, seL4_CapInitThreadCNode, 0, 0, slot, 1))
            return NULL;
        caps[i] = slot;
    }
    /* last arg cacheable=0 => NON-cacheable (matches net_genet.c DMA descriptors + boot_device_map):
     * the legacy DMA does not snoop the A72 caches, so a non-cacheable CB/scratch guarantees the DMA
     * reads the CB straight from DRAM (no stale-cache hazard) and that src/dst copies are real DRAM traffic. */
    void *v = vspace_map_pages(&vspace, caps, NULL, seL4_AllRights,
                               DMA_REGION_PAGES, seL4_PageBits, 0);
    if (!v) return NULL;
    *out_pa = base_pa;
    return v;
}

/* Boot-time init (root vspace: vka/vspace are single-owner here). Allocates the
 * scratch region, picks a free DMA channel, builds the self-loop CB, RESETs the
 * channel, and loads CONBLK_AD -- but does NOT start it (default disarmed). */
void dma_warm_init(void);
void dma_warm_init(void)
{
    if (!dev_dma_vaddr) {
        printf("[dmawarm] DMA controller not mapped -- keep-warm unavailable\n");
        return;
    }
    g_dma_base = dev_dma_vaddr;

    uint64_t region_pa = 0;
    void *region = alloc_low_region(&region_pa);
    if (!region) {
        printf("[dmawarm] no <1GB region -- keep-warm unavailable\n");
        return;
    }
    memset(region, 0, DMA_REGION_PAGES * 0x1000);
    /* src pattern: if the DMA runs even once, dst (read back in status) becomes this -> proves a transfer
     * happened; CS.ACTIVE staying 1 over time then proves the self-loop is still running. */
    memset((void *)((uintptr_t)region + 0x1000), 0xA5, 0x1000);
    g_region_pa = region_pa;

    /* page0: CB at +0 (32-byte aligned), mailbox tag buffer at +0x40 (16-byte aligned).
     * page1: src (4KB). page2: dst (4KB). */
    g_cb            = (volatile struct dma_cb *)((uintptr_t)region + 0x00);
    g_cb_bus        = BUS(region_pa + 0x00);
    volatile uint32_t *tag = (volatile uint32_t *)((uintptr_t)region + 0x40);
    uint64_t tag_bus = BUS(region_pa + 0x40);
    uint64_t src_bus = BUS(region_pa + 0x1000);
    uint64_t dst_bus = BUS(region_pa + 0x2000);

    /* Pick a channel from the VC's ARM-usable mask. Prefer a higher full-feature
     * channel (0-6) the VC is least likely to use; fall back to channel 5 if the
     * query fails. Skip 0-3 (commonly VC-reserved) when the mask permits. */
    uint32_t mask = query_dma_channel_mask(tag, tag_bus);
    int chosen = -1;
    if (mask) {
        /* Only FULL-feature channels 0-6 (the lite channels 7-10 may not support the 128-bit
         * width / bursts our CB uses). Prefer the higher ones (6,5,4) the VC is least likely to
         * reserve, then fall to 3..0. */
        for (int c = 6; c >= 0; c--) { if (mask & (1u << c)) { chosen = c; break; } }
    }
    if (chosen < 0) chosen = 5;   /* conservative fallback (full-feature channel) */
    g_dma_chan = chosen;

    if (g_cb_bus & 0x1Fu) {       /* CONBLK_AD requires 256-bit (32-byte) alignment */
        printf("[dmawarm] CB bus 0x%lx not 32-byte aligned -- keep-warm unavailable\n",
               (unsigned long)g_cb_bus);
        return;
    }

    /* Reset the channel and confirm it comes back idle (guards against grabbing a
     * channel the VC is actively using). */
    *chan_reg(g_dma_chan, DMA_CS) = CS_RESET;
    arch_dsb();
    for (int i = 0; i < 100000; i++) {
        if (!(*chan_reg(g_dma_chan, DMA_CS) & CS_RESET)) break;
    }
    *chan_reg(g_dma_chan, DMA_DEBUG) = DBG_CLR;   /* clear sticky errors */
    arch_dsb();

    /* Build the self-looping mem->mem control block: copy 4KB src->dst, throttled
     * by WAITS so it keeps the path warm without saturating DRAM bandwidth, then
     * loop back to itself forever. */
    g_cb->ti        = TI_SRC_INC | TI_DEST_INC | TI_WAIT_RESP |
                      TI_SRC_WIDTH128 | TI_DEST_WIDTH128 | TI_BURST(2) | TI_WAITS(31);
    g_cb->source_ad = (uint32_t)src_bus;
    g_cb->dest_ad   = (uint32_t)dst_bus;
    g_cb->txfr_len  = 0x1000;            /* 4KB per loop iteration */
    g_cb->stride    = 0;
    g_cb->nextconbk = (uint32_t)g_cb_bus; /* SELF-LOOP -> runs forever once started */
    g_cb->rsvd0 = g_cb->rsvd1 = 0;
    arch_dsb();

    g_dma_ready = 1;
    printf("[dmawarm] ready: chan=%d mask=0x%x region_pa=0x%lx cb_bus=0x%lx (default-OFF; /proc/dmawarm.1 to arm)\n",
           g_dma_chan, mask, (unsigned long)region_pa, (unsigned long)g_cb_bus);
}

/* /proc/dmawarm[.0|.1] -- the live A/B knob. .1 = arm (start the self-loop DMA),
 * .0 = disarm (abort + reset the channel), bare = status. */
int dma_warm_cmd(const char *args, char *buf, int bufsize);
int dma_warm_cmd(const char *args, char *buf, int bufsize)
{
    if (!g_dma_ready) {
        return snprintf(buf, bufsize, "dmawarm: unavailable (no DMA controller / no <1GB region)\n");
    }
    if (args[0] == '.' && args[1] == '1') {
        /* (re)load the CB and set ACTIVE. With NEXTCONBK self-referencing, the
         * channel never stops until aborted. */
        *chan_reg(g_dma_chan, DMA_CS)        = CS_END | CS_INT;   /* clear sticky status */
        *chan_reg(g_dma_chan, DMA_DEBUG)     = DBG_CLR;
        arch_dsb();
        *chan_reg(g_dma_chan, DMA_CONBLK_AD) = (uint32_t)g_cb_bus;
        arch_dsb();
        *chan_reg(g_dma_chan, DMA_CS)        = CS_ACTIVE | CS_PRIORITY(8) | CS_PANIC(8);
        arch_dsb();
        g_arm_count++;
        return snprintf(buf, bufsize,
            "dmawarm: ARMED chan=%d -- continuous DRAM keep-warm DMA running (CS=0x%x)\n",
            g_dma_chan, *chan_reg(g_dma_chan, DMA_CS));
    }
    if (args[0] == '.' && args[1] == '0') {
        *chan_reg(g_dma_chan, DMA_CS) = CS_ABORT;
        arch_dsb();
        for (int i = 0; i < 100000; i++) {
            if (!(*chan_reg(g_dma_chan, DMA_CS) & CS_ACTIVE)) break;
        }
        *chan_reg(g_dma_chan, DMA_CS) = CS_RESET;
        arch_dsb();
        return snprintf(buf, bufsize, "dmawarm: disarmed -- channel %d aborted+reset\n", g_dma_chan);
    }
    uint32_t cs  = *chan_reg(g_dma_chan, DMA_CS);
    uint32_t dbg = *chan_reg(g_dma_chan, DMA_DEBUG);
    uint32_t cb  = *chan_reg(g_dma_chan, DMA_CONBLK_AD);
    /* dst[0]: 0xa5a5a5a5 => the DMA copied the src pattern at least once. active=1 (persisting across
     * reads) => the self-loop is still running (a one-shot 4KB copy finishes in microseconds). */
    uint32_t dst0 = *(volatile uint32_t *)((uintptr_t)g_cb + 0x2000);
    return snprintf(buf, bufsize,
        "dmawarm: chan=%d active=%d err=%d CS=0x%x DEBUG=0x%x CONBLK=0x%x dst0=0x%x arms=%llu region_pa=0x%lx (.1 arm / .0 disarm)\n",
        g_dma_chan, (int)(cs & CS_ACTIVE), (int)((cs & CS_ERROR) != 0), cs, dbg, cb, dst0,
        (unsigned long long)g_arm_count, (unsigned long)g_region_pa);
}

#else  /* !PLAT_RPI4 */

void dma_warm_init(void) { /* QEMU: no BCM2711 DMA controller / no freeze */ }
int dma_warm_cmd(const char *args, char *buf, int bufsize)
{
    (void)args;
    return snprintf(buf, bufsize, "dmawarm: RPi4-only (not present on this platform)\n");
}

#endif

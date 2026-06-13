/*
 * net_genet.c -- BCM54213 GENET v5 Ethernet driver (RPi4)
 *
 * BCM2711 has an integrated GENET (Gigabit Ethernet Network) MAC
 * with an external BCM54213PE PHY connected via MDIO.
 *
 * Register region: 64KB (16 pages) at hw_info.genet_paddr.
 * DTB compatible: "brcm,bcm2711-genet-v5"
 *
 * DMA uses 256-entry descriptor rings (TX queue 16, RX queue 16).
 * Each descriptor is 12 bytes: length_status(4) + addr_lo(4) + addr_hi(4).
 * Descriptors must reference DMA-capable physical addresses.
 *
 * Phase 1: Polling mode. 100Mbps, no IRQ, PIO-style single-packet TX/RX.
 * Phase 2: IRQ-driven RX with net_rx_ring integration.
 *
 * Reference: Linux drivers/net/ethernet/broadcom/genet/
 *            Circle lib/bcmgenet.cpp
 */
#include "aios/root_shared.h"
#include "aios/net.h"
#include "aios/config.h"
#include "aios/vka_audit.h"
#include <sel4platsupport/device.h>
#include <stdio.h>
#include "aios/mono_wait.h"
#include <string.h>
#include "arch.h"
#include "aios/hw_info.h"
#include "aios/device_map.h"
#include "plat/net_hal.h"
#include "aios/netd_ctrl.h"   /* NETD_DIAG_* op codes (Stage 4 netdiag) */

/* ----------------------------------------------------------------
 * GENET register map (offsets within 64KB block)
 * ---------------------------------------------------------------- */

/* System registers */
#define SYS_REV_CTRL        0x0000
#define SYS_PORT_CTRL       0x0004
#define SYS_RBUF_FLUSH_CTRL 0x0008
#define SYS_TBUF_FLUSH_CTRL 0x000C

/* Port control bits */
#define PORT_MODE_INT_EPHY  0
#define PORT_MODE_INT_GPHY  1
#define PORT_MODE_EXT_EPHY  2
#define PORT_MODE_EXT_GPHY  3

/* RBUF (receive buffer control) */
#define RBUF_CTRL           0x0300
#define RBUF_ALIGN_2B       (1u << 1)   /* 2-byte alignment for IP headers */
#define RBUF_BAD_DIS        (1u << 2)   /* discard bad frames */
#define RBUF_TBUF_SIZE_CTRL 0x03B4      /* TBUF size control (set to 1 in reset) */

/* TBUF (transmit buffer control) */
#define TBUF_CTRL           0x0600

/* EXT block -- GENET<->external PHY RGMII out-of-band control */
#define GENET_EXT_OFF       0x0080
#define EXT_RGMII_OOB_CTRL  (GENET_EXT_OFF + 0x0C)  /* 0x008C */
#define RGMII_LINK          (1u << 4)
#define OOB_DISABLE         (1u << 5)
#define RGMII_MODE_EN       (1u << 6)
#define ID_MODE_DIS         (1u << 16)   /* MAC internal delay off (rgmii-rxid) */

/* INTRL2_0 -- CPU interrupt controller (RX/TX done, errors). Masked at boot;
 * unmask live for IRQ bring-up via cat /proc/genet.poke.214.<bits>. */
#define INTRL2_STAT         0x0200       /* asserted (post-mask) interrupt status */
#define INTRL2_CLEAR        0x0208       /* write-1-to-clear */
#define INTRL2_MASK_STATUS  0x020C       /* 1 = masked */
#define INTRL2_MASK_SET     0x0210       /* write-1-to-mask */
#define INTRL2_MASK_CLEAR   0x0214       /* write-1-to-unmask */

/* UniMAC registers (offset 0x0800) */
#define UMAC_BASE           0x0800
#define UMAC_CMD            (UMAC_BASE + 0x008)
#define UMAC_MAC0           (UMAC_BASE + 0x00C)  /* MAC[5:2] */
#define UMAC_MAC1           (UMAC_BASE + 0x010)  /* MAC[1:0] */
#define UMAC_MAX_FRAME      (UMAC_BASE + 0x014)
#define UMAC_MDIO_CMD       (UMAC_BASE + 0x614)
#define UMAC_TX_FLUSH       (UMAC_BASE + 0x334)  /* pulse 1 then 0 to flush TX */

/* UniMAC CMD bits */
#define CMD_TX_EN           (1u << 0)
#define CMD_RX_EN           (1u << 1)
#define CMD_SPEED_10        (0u << 2)
#define CMD_SPEED_100       (1u << 2)
#define CMD_SPEED_1000      (2u << 2)
#define CMD_PROMISC         (1u << 4)
#define CMD_PAD_EN          (1u << 5)
#define CMD_CRC_FWD         (1u << 6)
#define CMD_PAUSE_FWD       (1u << 7)
#define CMD_RX_PAUSE        (1u << 8)
#define CMD_TX_PAUSE        (1u << 9)
#define CMD_SW_RESET        (1u << 13)
#define CMD_LCL_LOOP        (1u << 15)

/* MDIO command bits */
#define MDIO_START_BUSY     (1u << 29)
#define MDIO_READ           (2u << 26)
#define MDIO_WRITE          (1u << 26)
#define MDIO_PMD_SHIFT      21
#define MDIO_REG_SHIFT      16

/* MIB counters (offset 0x0D00) -- not used in Phase 1 */

/* ----------------------------------------------------------------
 * GENET DMA register map (BCM2711 GENET v5)
 *
 * Layout corrected in v0.4.154 against the RPi4-proven U-Boot driver
 * (drivers/net/bcmgenet.c). The previous map was wrong on every axis --
 * descriptors past the 64KB MMIO window, control registers landing on top
 * of descriptor 0, RX prod/cons swapped -- so the DMA engines never ran.
 *
 * The descriptor RAM and the DMA registers all live INSIDE the 64KB
 * register block (genet_regs). Per direction (RX base 0x2000, TX 0x4000):
 *   - descriptor RAM:     256 slots * 12 bytes  (we use only ring 16)
 *   - per-ring registers: base + 256*12 + 16*0x40   (RX 0x3000, TX 0x5000)
 *   - global DMA regs:    base + 256*12 + 17*0x40   (RX 0x3040, TX 0x5040)
 *
 * Descriptor START/END/READ/WRITE pointers are in 32-bit WORD units
 * (3 words per 12-byte descriptor), not byte offsets.
 * ---------------------------------------------------------------- */
#define TOTAL_DESC          256         /* descriptor RAM slots per direction (HW) */
#define DMA_DESC_SIZE       12          /* length_status + addr_lo + addr_hi */
#define DMA_RING_SIZE       0x40        /* bytes per per-ring register set */
#define DEFAULT_Q           16          /* default ring index (DESC_INDEX) */

/* Descriptor RAM (in the register block) */
#define GENET_RX_DESC_BASE  0x2000
#define GENET_TX_DESC_BASE  0x4000

/* REG_OFF = desc_base + TOTAL_DESC*DMA_DESC_SIZE (registers follow the RAM) */
#define RDMA_REG_OFF        (GENET_RX_DESC_BASE + TOTAL_DESC * DMA_DESC_SIZE) /* 0x2C00 */
#define TDMA_REG_OFF        (GENET_TX_DESC_BASE + TOTAL_DESC * DMA_DESC_SIZE) /* 0x4C00 */

/* Per-ring register base for the default ring 16 */
#define RDMA_RING_BASE      (RDMA_REG_OFF + DEFAULT_Q * DMA_RING_SIZE)        /* 0x3000 */
#define TDMA_RING_BASE      (TDMA_REG_OFF + DEFAULT_Q * DMA_RING_SIZE)        /* 0x5000 */

/* Global DMA register base (after all 17 per-ring register sets) */
#define DMA_RINGS_SIZE      (DMA_RING_SIZE * (DEFAULT_Q + 1))                 /* 0x440 */
#define RDMA_CTRL_BASE      (RDMA_REG_OFF + DMA_RINGS_SIZE)                   /* 0x3040 */
#define TDMA_CTRL_BASE      (TDMA_REG_OFF + DMA_RINGS_SIZE)                   /* 0x5040 */

/* Per-ring register offsets. NOTE: TX and RX SWAP PROD/CONS:
 *   TX (TDMA): CONS at 0x08 (HW updates), PROD at 0x0C (SW updates)
 *   RX (RDMA): PROD at 0x08 (HW updates), CONS at 0x0C (SW updates) */
#define TDMA_READ_PTR       0x00
#define TDMA_CONS_INDEX     0x08
#define TDMA_PROD_INDEX     0x0C
#define RDMA_WRITE_PTR      0x00
#define RDMA_PROD_INDEX     0x08
#define RDMA_CONS_INDEX     0x0C
#define DMA_RING_BUF_SIZE   0x10        /* (num_descs << 16) | buf_len */
#define DMA_START_ADDR      0x14        /* word units */
#define DMA_END_ADDR        0x1C        /* word units */
#define DMA_MBUF_DONE_THR   0x24
#define DMA_XON_XOFF_THR    0x28        /* RX flow-control threshold */
#define TDMA_FLOW_PERIOD    0x28
#define TDMA_WRITE_PTR      0x2C
#define RDMA_READ_PTR       0x2C

/* Global DMA register offsets (from *_CTRL_BASE) */
#define DMA_RING_CFG        0x00        /* per-ring enable: 1 << ring_index */
#define DMA_CTRL            0x04
#define DMA_SCB_BURST_SIZE  0x0C

/* DMA_CTRL bits */
#define DMA_EN              (1u << 0)
#define DMA_RING_BUF_EN_SHIFT 1         /* ring i buffer enable = 1 << (i+1) */

/* DMA tunables (U-Boot DMA_MAX_BURST_LENGTH, ring-size shift, FC threshold) */
#define DMA_MAX_BURST_LENGTH 0x08
#define DMA_RING_SIZE_SHIFT 16
#define DMA_FC_THRESH_LO    5

/* DMA descriptor format (12 bytes each) */
struct genet_desc {
    uint32_t length_status;
    uint32_t addr_lo;
    uint32_t addr_hi;
};

/* Descriptor length_status bits */
#define DESC_OWN            (1u << 15)  /* 0x8000 owned by HW (set on RX init) */
#define DESC_EOP            (1u << 14)  /* 0x4000 end of packet */
#define DESC_SOP            (1u << 13)  /* 0x2000 start of packet */
#define DESC_WRAP           (1u << 12)  /* 0x1000 wrap ring */
#define DESC_TX_CRC         (1u << 6)   /* 0x0040 append CRC (TX) */
#define DESC_TX_QTAG_SHIFT  7           /* TX qtag field; must be 0x3F */
#define DESC_TX_QTAG_MASK   0x3F
#define DESC_LEN_SHIFT      16
#define DESC_LEN_MASK       0xFFF0000u

/* PHY registers (MDIO) */
#define PHY_ADDR            1   /* BCM54213 default PHY address */
#define MII_BMCR            0   /* Basic Mode Control */
#define MII_BMSR            1   /* Basic Mode Status */
#define MII_PHYID1          2
#define MII_PHYID2          3
#define MII_ANAR            4   /* Auto-Neg Advertisement */
#define MII_ANLPAR          5   /* Auto-Neg Link Partner Ability */

#define BMCR_RESET          (1u << 15)
#define BMCR_ANEG_EN        (1u << 12)
#define BMCR_ANEG_RESTART   (1u << 9)
#define BMCR_SPEED100       (1u << 13)
#define BMCR_FULL_DUPLEX    (1u << 8)

#define BMSR_LINK           (1u << 2)
#define BMSR_ANEG_DONE      (1u << 5)

/* ----------------------------------------------------------------
 * Driver state
 * ---------------------------------------------------------------- */
#define GENET_NUM_PAGES     16  /* 64KB / 4KB */
#define GENET_RX_DESCS      16
#define GENET_TX_DESCS      16

static volatile uint32_t *genet_regs;
static int genet_initialized;

/* v0.4.239 (netd Stage 3 HW fix): 1 while plat_net_prov runs. The prov path reads
 * the board MAC via the VC mailbox but must NOT touch GENET UMAC registers -- root
 * does not map genet_regs at prov (it is NULL; netd owns the device programming),
 * and SWINIT is still latched (a UMAC write would bus-error). netd programs UMAC
 * from the argv MAC after its OWN SWINIT release. The #ifndef NETD_PROV guard alone
 * does not cover this because the flag-ON root keeps the FULL driver (no NETD_PROV
 * define), so it reaches the UMAC writes via plat_net_prov -> read_mac_from_mailbox
 * with genet_regs NULL -> fault at 0x80c (UMAC_MAC0). This runtime flag gates them. */
static int genet_in_prov;

/* DMA buffer memory (allocated from VKA) */
static uint8_t  *genet_dma;
static uint64_t  genet_dma_pa;

/* DMA layout within allocated DMA buffer (128KB) */
#define GENET_DMA_SIZE       0x20000
#define GENET_DMA_FRAMES     32
#define GENET_RX_BUF_OFF     0x00000   /* 16 * 2048 = 32KB */
#define GENET_TX_BUF_OFF     0x08000   /* 16 * 2048 = 32KB */
#define GENET_PKT_BUF_SIZE   2048

/* Ring indices */
static uint16_t rx_prod_idx;
static uint16_t rx_cons_idx;
static uint16_t tx_prod_idx;
static uint16_t tx_cons_idx;

/* IRQ */
static seL4_CPtr genet_irq_handler;

/* v0.4.157: IRQ instrumentation -- INTRL2 is masked at boot (RX stays polled,
 * no regression). Unmask live (poke 0x214) and watch these climb to prove the
 * GENET RX interrupt fires before switching the driver to IRQ-driven. */
static volatile uint32_t genet_irq_count;
static volatile uint32_t genet_last_intstat;

/* v0.4.159/162: RX IRQ status flag (HW-verified IRQ-driven RX since v0.4.162).
 * 1 = INTRL2 unmasked (RX IRQs wake the merged net_server's bound notification);
 * 0 = masked. Toggle live via /proc/genet.irqon / .irqoff. v0.4.230 merged the
 * driver into net_server, so the old poll-mode loop is gone -- this is now a
 * status indicator for the INTRL2 mask, not a poll-vs-IRQ selector. */
static volatile int net_rx_irq_mode = 1;

/* MAC address storage */
static uint8_t genet_mac[6];

/* ----------------------------------------------------------------
 * Register access
 * ---------------------------------------------------------------- */
#define GENET_R(off)      (genet_regs[(off) / 4])
#define GENET_W(off, val) do { genet_regs[(off) / 4] = (val); arch_dsb(); } while (0)

static void genet_delay(int us) {
    for (volatile int i = 0; i < us * 100; i++) {
        asm volatile("" ::: "memory");
    }
}

/* ----------------------------------------------------------------
 * MDIO read/write -- access BCM54213 PHY registers
 * ---------------------------------------------------------------- */
static uint16_t mdio_read(int phy, int reg) {
    uint32_t cmd = MDIO_START_BUSY | MDIO_READ |
                   ((uint32_t)phy << MDIO_PMD_SHIFT) |
                   ((uint32_t)reg << MDIO_REG_SHIFT);
    GENET_W(UMAC_MDIO_CMD, cmd);

    /* Wait for completion */
    for (uint64_t dl = mono_deadline_ms(2000); mono_before(dl); ) {
        arch_dmb();
        uint32_t v = GENET_R(UMAC_MDIO_CMD);
        if (!(v & MDIO_START_BUSY))
            return (uint16_t)(v & 0xFFFF);
    }
    printf("[net] MDIO read timeout (phy=%d reg=%d)\n", phy, reg);
    return 0xFFFF;
}

static void mdio_write(int phy, int reg, uint16_t val) {
    uint32_t cmd = MDIO_START_BUSY | MDIO_WRITE |
                   ((uint32_t)phy << MDIO_PMD_SHIFT) |
                   ((uint32_t)reg << MDIO_REG_SHIFT) |
                   (uint32_t)val;
    GENET_W(UMAC_MDIO_CMD, cmd);

    for (uint64_t dl = mono_deadline_ms(2000); mono_before(dl); ) {
        arch_dmb();
        if (!(GENET_R(UMAC_MDIO_CMD) & MDIO_START_BUSY))
            return;
    }
    printf("[net] MDIO write timeout (phy=%d reg=%d)\n", phy, reg);
}

/* ----------------------------------------------------------------
 * phy_init -- reset and configure BCM54213 PHY
 * ---------------------------------------------------------------- */
static int phy_init(void) {
    /* Reset PHY */
    mdio_write(PHY_ADDR, MII_BMCR, BMCR_RESET);
    genet_delay(50000);

    /* Wait for reset to clear */
    for (int t = 0; t < 100; t++) {
        uint16_t bmcr = mdio_read(PHY_ADDR, MII_BMCR);
        if (!(bmcr & BMCR_RESET)) break;
        genet_delay(10000);
    }

    /* Read PHY ID for verification */
    uint16_t id1 = mdio_read(PHY_ADDR, MII_PHYID1);
    uint16_t id2 = mdio_read(PHY_ADDR, MII_PHYID2);
    printf("[net] PHY ID: 0x%04x:0x%04x\n", id1, id2);

    if (id1 == 0xFFFF || id1 == 0x0000) {
        printf("[net] No PHY at address %d\n", PHY_ADDR);
        return -1;
    }

    /* Force 100Mbps full duplex (Phase 1, no autoneg) */
    mdio_write(PHY_ADDR, MII_BMCR,
               BMCR_SPEED100 | BMCR_FULL_DUPLEX);
    genet_delay(50000);

    /* Wait for link */
    for (int t = 0; t < 200; t++) {
        uint16_t bmsr = mdio_read(PHY_ADDR, MII_BMSR);
        if (bmsr & BMSR_LINK) {
            printf("[net] PHY link up (100Mbps FD)\n");
            return 0;
        }
        genet_delay(50000);
    }

    printf("[net] PHY link timeout (no cable?)\n");
    return -1;
}

/* ----------------------------------------------------------------
 * read_mac_from_umac -- read MAC address from UniMAC registers
 *
 * The firmware programs the MAC address into UMAC_MAC0/MAC1
 * during boot. We read it from there rather than DTB.
 * ---------------------------------------------------------------- */
static void read_mac_from_umac(void) {
    arch_dmb();
    uint32_t mac0 = GENET_R(UMAC_MAC0);
    uint32_t mac1 = GENET_R(UMAC_MAC1);

    genet_mac[0] = (uint8_t)(mac0 >> 24);
    genet_mac[1] = (uint8_t)(mac0 >> 16);
    genet_mac[2] = (uint8_t)(mac0 >>  8);
    genet_mac[3] = (uint8_t)(mac0 >>  0);
    genet_mac[4] = (uint8_t)(mac1 >> 8);   /* UMAC_MAC1 is a 16-bit field (low bits) */
    genet_mac[5] = (uint8_t)(mac1 >> 0);

    /* Copy to global net_mac for stack */
    for (int i = 0; i < 6; i++) net_mac[i] = genet_mac[i];
}

/* ================================================================
 * Provisioning half (root only) -- DMA alloc, VC-mailbox MAC query, IRQ bind.
 * Gated #ifndef NETD_BUILD: these touch vka / the VC mailbox, so netd never
 * compiles them; netd receives the DMA region, MAC, and IRQ handler via
 * plat_net_dev_attach() (DESIGN_NETD s3/s7). The device-register sequence
 * (SWINIT/UMAC/RBUF/RGMII/ring_init/INTRL2 in plat_net_init) is the dev half
 * and stays byte-identical -- only its prov sub-steps are gated.
 * ================================================================ */
#ifndef NETD_BUILD
#include "aios/netd_handoff.h"

/* Retained DMA frame caps (32 x 4K) for the netd handoff (DESIGN_NETD s3). */
static seL4_CPtr genet_dma_caps[GENET_DMA_FRAMES];

/* ----------------------------------------------------------------
 * dma_init -- allocate the 128KB DMA region BELOW 1GB.
 *
 * v0.4.236 (netd Stage 3b, DESIGN_NETD s3): retry-for-low <1GB loop (the
 * HW-proven xhci_dma_reserve pattern, src/usb/xhci.c). genet_dma_pa MUST land
 * below GENET_DMA_LIMIT: the VC-mailbox bus alias ORs 0xC0000000 and truncates to
 * 32 bits (read_mac_from_mailbox), and the GENET SCB master wants a low region;
 * the allocator provably hands out >3.9GB frames late in boot. Each attempt
 * retypes one PROBE frame to read the untyped base paddr; a too-high untyped is
 * kept allocated as a reject (so the next alloc differs) and freed at the end.
 * Fail LOUD on no low region -- never silently fall back. RPi4-only (net_genet is
 * not built for QEMU, whose RAM base is itself 0x40000000).
 * ---------------------------------------------------------------- */
#define GENET_DMA_LIMIT 0x40000000ULL   /* keep paddr | 0xC0000000 within 32 bits */

static int dma_init(void) {
    int error;
    vka_object_t dma_ut;
    vka_object_t rejects[8];
    int nrej = 0, have = 0;

    /* Find a 128KB untyped (size 17) whose base lands below 1GB. Probe each by
     * retyping frame 0 and reading its paddr; keep frame 0 of the winning untyped. */
    for (int t = 0; t < 8 && !have; t++) {
        vka_audit_untyped(VKA_SUB_NET, 17);
        if (vka_alloc_untyped(&vka, 17, &dma_ut)) break;

        seL4_CPtr probe;
        if (vka_cspace_alloc(&vka, &probe)) {
            vka_free_object(&vka, &dma_ut);
            break;
        }
        error = seL4_Untyped_Retype(dma_ut.cptr, ARCH_PAGE_OBJECT, seL4_PageBits,
                                    seL4_CapInitThreadCNode, 0, 0, probe, 1);
        if (error) {
            printf("[net] DMA probe retype failed: %d\n", error);
            vka_cspace_free(&vka, probe);
            vka_free_object(&vka, &dma_ut);
            break;
        }
        seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(probe);
        if (!ga.error && ga.paddr + GENET_DMA_SIZE <= GENET_DMA_LIMIT) {
            genet_dma_caps[0] = probe;     /* frame 0 of the low untyped */
            genet_dma_pa = ga.paddr;
            have = 1;
            break;
        }
        /* Too high: delete the probe frame, keep the untyped as a reject so the
         * next alloc lands elsewhere, retry. */
        seL4_CNode_Delete(seL4_CapInitThreadCNode, probe, seL4_WordBits);
        vka_cspace_free(&vka, probe);
        if (nrej < 8) rejects[nrej++] = dma_ut;
        else          vka_free_object(&vka, &dma_ut);
    }

    for (int i = 0; i < nrej; i++) vka_free_object(&vka, &rejects[i]);

    if (!have) {
        printf("[net] DMA: no <1GB region found in 8 tries -- net unavailable\n");
        return -1;
    }

    /* Retype the remaining 31 frames from the same (low) untyped -- contiguous
     * after frame 0, so genet_dma_pa is the base of the whole 128KB region. */
    for (int i = 1; i < GENET_DMA_FRAMES; i++) {
        seL4_CPtr slot;
        error = vka_cspace_alloc(&vka, &slot);
        if (error) {
            printf("[net] DMA cslot alloc failed at %d\n", i);
            return -1;
        }
        error = seL4_Untyped_Retype(dma_ut.cptr,
            ARCH_PAGE_OBJECT, seL4_PageBits,
            seL4_CapInitThreadCNode, 0, 0, slot, 1);
        if (error) {
            printf("[net] DMA retype %d failed: %d\n", i, error);
            return -1;
        }
        genet_dma_caps[i] = slot;
    }

    void *dma_vaddr = vspace_map_pages(&vspace, genet_dma_caps, NULL,
        seL4_AllRights, GENET_DMA_FRAMES, seL4_PageBits, 0);
    if (!dma_vaddr) {
        printf("[net] DMA map failed\n");
        return -1;
    }

    genet_dma = (uint8_t *)dma_vaddr;
    memset(genet_dma, 0, GENET_DMA_SIZE);

    printf("[net] DMA region: virt=%p phys=0x%lx (128KB, <1GB, %d reject(s))\n",
           dma_vaddr, (unsigned long)genet_dma_pa, nrej);
    return 0;
}

#endif /* !NETD_BUILD (dma_init) */

/* ----------------------------------------------------------------
 * ring_init -- configure RX and TX default rings (ring 16). Dev half: MMIO-only,
 * runs in netd. Uses genet_dma_pa (attached from prov via argv in the netd path).
 * ---------------------------------------------------------------- */
static void ring_init(void) {
    /* --- Disable both DMA engines first (U-Boot bcmgenet_disable_dma) --- */
    GENET_W(TDMA_CTRL_BASE + DMA_CTRL,
            GENET_R(TDMA_CTRL_BASE + DMA_CTRL) & ~DMA_EN);
    GENET_W(RDMA_CTRL_BASE + DMA_CTRL,
            GENET_R(RDMA_CTRL_BASE + DMA_CTRL) & ~DMA_EN);
    GENET_W(UMAC_TX_FLUSH, 1);
    genet_delay(10);
    GENET_W(UMAC_TX_FLUSH, 0);
    genet_delay(10);

    /* ============================================================
     * RX ring 16 (default). Descriptors live in the register block at
     * GENET_RX_DESC_BASE; each points to a DMA buffer in DRAM. Hand every
     * descriptor to HW (DESC_OWN) with its buffer length.
     * ============================================================ */
    volatile struct genet_desc *rx_descs =
        (volatile struct genet_desc *)((uintptr_t)genet_regs + GENET_RX_DESC_BASE);
    for (int i = 0; i < GENET_RX_DESCS; i++) {
        uint64_t buf_pa = genet_dma_pa + GENET_RX_BUF_OFF +
                          (uint64_t)i * GENET_PKT_BUF_SIZE;
        rx_descs[i].addr_lo = (uint32_t)buf_pa;
        rx_descs[i].addr_hi = (uint32_t)(buf_pa >> 32);
        rx_descs[i].length_status =
            (GENET_PKT_BUF_SIZE << DESC_LEN_SHIFT) | DESC_OWN;
    }
    arch_dsb();

    GENET_W(RDMA_CTRL_BASE + DMA_SCB_BURST_SIZE, DMA_MAX_BURST_LENGTH);

    /* Ring window in WORD units: [0, descs*12/4 - 1]. */
    GENET_W(RDMA_RING_BASE + DMA_START_ADDR, 0);
    GENET_W(RDMA_RING_BASE + RDMA_READ_PTR, 0);
    GENET_W(RDMA_RING_BASE + RDMA_WRITE_PTR, 0);
    GENET_W(RDMA_RING_BASE + DMA_END_ADDR,
            GENET_RX_DESCS * DMA_DESC_SIZE / 4 - 1);

    /* RX producer is HW-owned: read it, align our consumer to it, and do NOT
     * write the producer. Indices are free-running 16-bit; descriptor index =
     * counter % GENET_RX_DESCS (16 divides 65536 exactly). */
    {
        uint16_t p = (uint16_t)(GENET_R(RDMA_RING_BASE + RDMA_PROD_INDEX) & 0xFFFF);
        GENET_W(RDMA_RING_BASE + RDMA_CONS_INDEX, p);
        rx_prod_idx = p;
        rx_cons_idx = p;
    }
    GENET_W(RDMA_RING_BASE + DMA_RING_BUF_SIZE,
            (GENET_RX_DESCS << DMA_RING_SIZE_SHIFT) | GENET_PKT_BUF_SIZE);
    GENET_W(RDMA_RING_BASE + DMA_XON_XOFF_THR,
            (DMA_FC_THRESH_LO << 16) | (GENET_RX_DESCS >> 4));
    GENET_W(RDMA_CTRL_BASE + DMA_RING_CFG, 1u << DEFAULT_Q);  /* activate ring 16 */

    /* ============================================================
     * TX ring 16 (default). Descriptors are filled on demand in plat_net_tx.
     * ============================================================ */
    GENET_W(TDMA_CTRL_BASE + DMA_SCB_BURST_SIZE, DMA_MAX_BURST_LENGTH);

    GENET_W(TDMA_RING_BASE + DMA_START_ADDR, 0);
    GENET_W(TDMA_RING_BASE + TDMA_READ_PTR, 0);
    GENET_W(TDMA_RING_BASE + TDMA_WRITE_PTR, 0);
    GENET_W(TDMA_RING_BASE + DMA_END_ADDR,
            GENET_TX_DESCS * DMA_DESC_SIZE / 4 - 1);

    /* TX consumer is HW-owned: read it, align our producer to it. */
    {
        uint16_t c = (uint16_t)(GENET_R(TDMA_RING_BASE + TDMA_CONS_INDEX) & 0xFFFF);
        GENET_W(TDMA_RING_BASE + TDMA_PROD_INDEX, c);
        tx_prod_idx = c;
        tx_cons_idx = c;
    }
    GENET_W(TDMA_RING_BASE + DMA_MBUF_DONE_THR, 1);
    GENET_W(TDMA_RING_BASE + TDMA_FLOW_PERIOD, 0);
    GENET_W(TDMA_RING_BASE + DMA_RING_BUF_SIZE,
            (GENET_TX_DESCS << DMA_RING_SIZE_SHIFT) | GENET_PKT_BUF_SIZE);
    GENET_W(TDMA_CTRL_BASE + DMA_RING_CFG, 1u << DEFAULT_Q);  /* activate ring 16 */

    /* ============================================================
     * Enable both DMA engines: global DMA_EN + ring-16 buffer enable.
     * RING_CFG uses bit DEFAULT_Q (16); CTRL uses bit DEFAULT_Q+1 (17).
     * ============================================================ */
    {
        uint32_t dma_ctrl = (1u << (DEFAULT_Q + DMA_RING_BUF_EN_SHIFT)) | DMA_EN;
        GENET_W(TDMA_CTRL_BASE + DMA_CTRL, dma_ctrl);
        GENET_W(RDMA_CTRL_BASE + DMA_CTRL,
                GENET_R(RDMA_CTRL_BASE + DMA_CTRL) | dma_ctrl);
    }
    arch_dsb();
}

#ifndef NETD_BUILD   /* prov half: VC mailbox + IRQ bind (root/vka/vcmbox) */

/* VC property-mailbox call (channel 8). buf_pa = VC bus address of a 16-byte-
 * aligned tag buffer; buf = ARM pointer to read the response. From display_vc.c. */
static int genet_mbox_call(uint64_t buf_pa, volatile uint32_t *buf) {
    if (!dev_vcmbox_vaddr) return -1;
    volatile uint32_t *mbox =
        (volatile uint32_t *)((uintptr_t)dev_vcmbox_vaddr + dev_vcmbox_off);
    uint32_t addr_ch = (uint32_t)(buf_pa & 0xFFFFFFF0u) | 8u;
    for (uint64_t dl = mono_deadline_ms(2000); mono_before(dl); ) {
        arch_dmb();
        if (!(mbox[0x18 / 4] & 0x80000000u)) break;     /* not FULL */
    }
    arch_dsb();
    mbox[0x20 / 4] = addr_ch;                            /* WRITE */
    arch_dsb();
    for (uint64_t dl = mono_deadline_ms(2000); mono_before(dl); ) {
        arch_dmb();
        if (mbox[0x18 / 4] & 0x40000000u) continue;     /* EMPTY */
        arch_dmb();
        if (mbox[0x00 / 4] == addr_ch)                   /* our response */
            return (buf[1] == 0x80000000u) ? 0 : -1;
    }
    return -1;
}

/* v0.4.158: read the REAL board MAC via the VC firmware mailbox
 * (PROPTAG_GET_MAC_ADDRESS) and program it into genet_mac/net_mac/UMAC. Uses a
 * slice of the non-cacheable DMA buffer as the tag buffer (VC bus alias). Returns
 * 0 on success; -1 on any failure (caller keeps the existing MAC). Needs genet_dma
 * (call after dma_init). HW-confirmed: returns dc:a6:32:xx:xx:xx. */
static int read_mac_from_mailbox(void) {
    if (!dev_vcmbox_vaddr || !genet_dma) return -1;
    volatile uint32_t *m = (volatile uint32_t *)(genet_dma + 0x10000);
    uint64_t bus = (genet_dma_pa + 0x10000) | 0xC0000000ULL;
    m[0] = 32; m[1] = 0; m[2] = 0x00010003u; m[3] = 8; m[4] = 0;
    m[5] = 0; m[6] = 0; m[7] = 0;
    arch_dsb();
    if (genet_mbox_call(bus, m) != 0) return -1;
    arch_dmb();
    uint32_t lo = m[5], hi = m[6];
    uint8_t mac[6] = {
        (uint8_t)(lo & 0xFF), (uint8_t)((lo >> 8) & 0xFF),
        (uint8_t)((lo >> 16) & 0xFF), (uint8_t)((lo >> 24) & 0xFF),
        (uint8_t)(hi & 0xFF), (uint8_t)((hi >> 8) & 0xFF)
    };
    if ((mac[0]|mac[1]|mac[2]|mac[3]|mac[4]|mac[5]) == 0) return -1;
    for (int i = 0; i < 6; i++) { genet_mac[i] = mac[i]; net_mac[i] = mac[i]; }
#ifndef NETD_PROV
    /* Program the MAC into UMAC. SKIPPED at prov time (genet_in_prov): root has not
     * mapped genet_regs (it is NULL) and SWINIT is still latched, so a UMAC write
     * would fault / bus-error -> kernel halt (v0.4.151/239). netd programs UMAC from
     * this MAC (handed over via argv) in plat_net_init() AFTER its own SWINIT release
     * + UMAC reset; the monolithic build reaches here only after SWINIT is already
     * cleared (genet_in_prov stays 0), so the write is safe. DESIGN_NETD s7. */
    if (!genet_in_prov) {
        GENET_W(UMAC_MAC0, ((uint32_t)mac[0] << 24) | ((uint32_t)mac[1] << 16) |
                           ((uint32_t)mac[2] << 8) | mac[3]);
        /* v0.4.161: UMAC_MAC1 is a 16-bit field -- bytes 4,5 go in the LOW half
         * (the upper 16 bits are reserved). The old <<24/<<16 packing landed them in
         * the reserved half -> MAC1 read back 0 -> unicast RX filter was
         * dc:a6:32:xx:00:00 -> pings (unicast) were dropped while broadcast worked. */
        GENET_W(UMAC_MAC1, ((uint32_t)mac[4] << 8) | (uint32_t)mac[5]);
    }
#endif
    return 0;
}

/* genet_irq_bind -- allocate the RX IRQ notification + a badge=2 kick copy and
 * bind the GENET IRQ to a badge=1 minted copy (v0.4.230/162). Factored from
 * plat_net_init for the netd prov path; netd inherits genet_irq_handler via
 * plat_net_dev_attach() and self-binds the unbadged ntfn to its own TCB. */
static int genet_irq_bind(void) {
    int error;
    /* RX IRQ notification (v0.4.230 Stage 1: bound to the net_server TCB in
     * boot_services; the old separate net_srv_ntfn / driver thread are gone). */
    vka_object_t drv_ntfn_obj;
    error = vka_alloc_notification(&vka, &drv_ntfn_obj);
    if (error) {
        printf("[net] drv notification alloc failed\n");
        return -1;
    }
    net_drv_ntfn_cap = drv_ntfn_obj.cptr;

    /* Mint a badge=2 RX-kick copy of net_drv_ntfn for /proc/genet.irqoff (the
     * unwedge that wakes the merged net_server when its IRQ path is masked). */
    {
        cspacepath_t ksrc, k2;
        vka_cspace_make_path(&vka, net_drv_ntfn_cap, &ksrc);
        if (!vka_cspace_alloc_path(&vka, &k2) &&
            !seL4_CNode_Mint(k2.root, k2.capPtr, k2.capDepth,
                             ksrc.root, ksrc.capPtr, ksrc.capDepth,
                             seL4_AllRights, (seL4_Word)2))
            net_kick_ntfn_cap = k2.capPtr;
    }

    /* Bind GENET IRQ to a badge=1 minted copy so the merged net_server's
     * seL4_Recv wakes with a non-zero badge (its IRQ wake test). The unbadged
     * net_drv_ntfn_cap stays bound to the server TCB. */
    {
        cspacepath_t irq_path;
        int irq_err = vka_cspace_alloc_path(&vka, &irq_path);
        if (!irq_err) {
            irq_err = simple_get_IRQ_handler(&simple, hw_info.genet_irq,
                                              irq_path);
            if (!irq_err) {
                genet_irq_handler = irq_path.capPtr;
                seL4_CPtr irq_ntfn = net_drv_ntfn_cap;
                cspacepath_t b1src, b1;
                vka_cspace_make_path(&vka, net_drv_ntfn_cap, &b1src);
                if (!vka_cspace_alloc_path(&vka, &b1) &&
                    !seL4_CNode_Mint(b1.root, b1.capPtr, b1.capDepth,
                                     b1src.root, b1src.capPtr, b1src.capDepth,
                                     seL4_AllRights, (seL4_Word)1))
                    irq_ntfn = b1.capPtr;
                irq_err = seL4_IRQHandler_SetNotification(
                    genet_irq_handler, irq_ntfn);
                if (!irq_err) {
                    seL4_IRQHandler_Ack(genet_irq_handler);
                    printf("[net] IRQ %u bound to driver\n",
                           hw_info.genet_irq);
                } else {
                    printf("[net] IRQ bind failed: %d\n", irq_err);
                }
            } else {
                printf("[net] IRQ handler failed: %d (irq=%u)\n",
                       irq_err, hw_info.genet_irq);
            }
        }
    }
    return 0;
}

/* plat_net_prov -- root-side provisioning for the flag-ON netd path
 * (DESIGN_NETD s7): allocate DMA, read the board MAC via the VC mailbox (bytes
 * only -- the UMAC programming is gated out at prov time), bind the IRQ. netd
 * runs the GENET register sequence itself in plat_net_init() after
 * plat_net_dev_attach(). */
int plat_net_prov(driver_handoff_t *ho) {
    if (!hw_info.has_genet) {
        printf("[net] No GENET in DTB\n");
        return -1;
    }
    /* prov reads the MAC via the mailbox but must not touch UMAC (genet_regs is
     * NULL here + SWINIT latched); netd programs UMAC after its own reset. */
    genet_in_prov = 1;
    if (dma_init() != 0) {
        printf("[net] DMA init failed\n");
        return -1;
    }
    if (read_mac_from_mailbox() == 0)
        printf("[net] real MAC (mailbox, prov): %02x:%02x:%02x:%02x:%02x:%02x\n",
               net_mac[0], net_mac[1], net_mac[2], net_mac[3], net_mac[4], net_mac[5]);
    else
        printf("[net] mailbox MAC read failed at prov, keeping fallback\n");
    if (genet_irq_bind() != 0) return -1;

    /* Fill the handoff for spawn_netd (DESIGN_NETD s3). GENET is one fixed device:
     * the 16 MMIO frame caps come from boot_device_map (dev_genet_frame_caps);
     * netd maps them at dev_genet_vaddr and uses that as genet_regs (slot 0). The
     * MAC was read here via the mailbox (bytes only -- UMAC programming is gated
     * out at prov); netd programs UMAC from it after its own SWINIT release. */
    for (int i = 0; i < GENET_NUM_PAGES; i++)
        ho->mmio_frames[i] = dev_genet_frame_caps[i];
    ho->mmio_nframes = GENET_NUM_PAGES;
    ho->mmio_vaddr   = (uintptr_t)dev_genet_vaddr;
    ho->slot         = 0;
    for (int i = 0; i < GENET_DMA_FRAMES; i++)
        ho->dma_frames[i] = genet_dma_caps[i];
    ho->dma_nframes  = GENET_DMA_FRAMES;
    ho->dma_vaddr    = (uintptr_t)genet_dma;
    ho->dma_paddr    = genet_dma_pa;
    ho->irq_handler  = genet_irq_handler;
    ho->irq_ntfn     = net_drv_ntfn_cap;
    for (int i = 0; i < 6; i++) ho->mac[i] = genet_mac[i];
    return 0;
}

#endif /* !NETD_BUILD (prov half) */

#ifdef NETD_BUILD
/* netd latches the root-provisioned MMIO/DMA/IRQ/MAC (received via argv) before
 * plat_net_init() runs the GENET register sequence. DESIGN_NETD s3. */
void plat_net_dev_attach(uintptr_t mmio_vaddr, int slot, uintptr_t dma_vaddr,
                         uint64_t dma_paddr, seL4_CPtr irq_handler,
                         const uint8_t mac[6]) {
    (void)slot;   /* GENET is a single fixed device; no slot index */
    genet_regs = (volatile uint32_t *)mmio_vaddr;
    genet_dma = (uint8_t *)dma_vaddr;
    genet_dma_pa = dma_paddr;
    genet_irq_handler = irq_handler;
    for (int i = 0; i < 6; i++) { genet_mac[i] = mac[i]; net_mac[i] = mac[i]; }
}
#endif

/* ================================================================
 * plat_net_init -- run the GENET device-register sequence (dev half).
 *
 * Monolithic (flag-OFF, neither define): provisions inline via the
 * #ifndef NETD_BUILD helpers, in the original order, then programs the device --
 * behavior unchanged. netd (NETD_BUILD): the prov helpers are gated out; netd
 * has already attached the root-provisioned MMIO/DMA/IRQ/MAC.
 * ================================================================ */
#ifndef NETD_PROV
int plat_net_init(void) {
    /* v0.4.149: re-enabled -- the v0.4.98 disable was a misattributed
     * device-MMIO watermark bug, now fixed by prealloc_rpi4_devices(). */
#ifndef NETD_BUILD
    if (!hw_info.has_genet) {
        printf("[net] No GENET in DTB\n");
        return -1;
    }

    printf("[net] GENET at 0x%lx IRQ %u\n",
           (unsigned long)hw_info.genet_paddr, hw_info.genet_irq);

    /* v0.4.149: use the pre-mapped GENET MMIO (claimed ascending in
     * prealloc_rpi4_devices so it lands ahead of the higher peripherals
     * instead of behind the device-untyped watermark). */
    if (!dev_genet_vaddr) {
        printf("[net] GENET MMIO not pre-mapped\n");
        return -1;
    }
    genet_regs = dev_genet_vaddr;
#endif /* !NETD_BUILD: netd attached genet_regs via plat_net_dev_attach() */

    /* Verify controller is alive */
    arch_dmb();
    uint32_t rev = GENET_R(SYS_REV_CTRL);
    if (rev == 0 || rev == 0xFFFFFFFF) {
        printf("[net] GENET not responding (rev=0x%x)\n", rev);
        return -1;
    }
    uint32_t major = (rev >> 24) & 0xFF;
    uint32_t minor = (rev >> 16) & 0xFF;
    printf("[net] GENET rev: 0x%08x (v%u.%u)\n", rev, major, minor);

    /* v0.4.151: release the UMAC software-reset latch BEFORE touching any UMAC
     * register. The GENET powers up with SYS_RBUF_FLUSH_CTRL.SWINIT set, which
     * holds the UMAC sub-block in reset; accessing UMAC (0x808) while it is held
     * in reset bus-errors -> external abort -> kernel halt. NOT clock-gating (the
     * firmware clocks GENET; the rev read above proves the SYS block is live).
     * HW-CONFIRMED on real RPi4 (v0.4.151). Linux/Circle reset_umac do the same
     * via rbuf_ctrl_set(0). If GENET ever halts right after the rev print above,
     * this clear is the first thing to check. */
    GENET_W(SYS_RBUF_FLUSH_CTRL, 0);
    genet_delay(10);

    /* --- UniMAC reset (Linux/Circle reset_umac order) --- */
    GENET_W(UMAC_CMD, 0);
    GENET_W(UMAC_CMD, CMD_SW_RESET | CMD_LCL_LOOP);
    genet_delay(2);
    GENET_W(UMAC_CMD, 0);
    genet_delay(10);

#ifdef NETD_BUILD
    /* netd: root read the board MAC via the VC mailbox at prov time and handed it
     * over via argv (latched in plat_net_dev_attach). Program it into UMAC now
     * that SWINIT is released + UMAC reset. If it was zero, fall back to whatever
     * the firmware left in UMAC. v0.4.161 MAC1 16-bit low-half packing. */
    if (!(genet_mac[0] | genet_mac[1] | genet_mac[2] |
          genet_mac[3] | genet_mac[4] | genet_mac[5]))
        read_mac_from_umac();
    GENET_W(UMAC_MAC0, ((uint32_t)genet_mac[0] << 24) | ((uint32_t)genet_mac[1] << 16) |
                       ((uint32_t)genet_mac[2] <<  8) | ((uint32_t)genet_mac[3]));
    GENET_W(UMAC_MAC1, ((uint32_t)genet_mac[4] << 8) | ((uint32_t)genet_mac[5]));
    for (int i = 0; i < 6; i++) net_mac[i] = genet_mac[i];
    printf("[net] MAC (prov): %02x:%02x:%02x:%02x:%02x:%02x\n",
           genet_mac[0], genet_mac[1], genet_mac[2],
           genet_mac[3], genet_mac[4], genet_mac[5]);
#else
    /* Read MAC address from UniMAC (set by firmware) */
    read_mac_from_umac();
    printf("[net] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           genet_mac[0], genet_mac[1], genet_mac[2],
           genet_mac[3], genet_mac[4], genet_mac[5]);

    /* If MAC is all zeros, firmware did not set it */
    if (genet_mac[0] == 0 && genet_mac[1] == 0 &&
        genet_mac[2] == 0 && genet_mac[3] == 0 &&
        genet_mac[4] == 0 && genet_mac[5] == 0) {
        printf("[net] MAC not set by firmware, using fallback\n");
        genet_mac[0] = 0xDC; genet_mac[1] = 0xA6;
        genet_mac[2] = 0x32; genet_mac[3] = 0x01;
        genet_mac[4] = 0x02; genet_mac[5] = 0x03;
        uint32_t m0 = ((uint32_t)genet_mac[0] << 24) |
                      ((uint32_t)genet_mac[1] << 16) |
                      ((uint32_t)genet_mac[2] <<  8) |
                      ((uint32_t)genet_mac[3]);
        uint32_t m1 = ((uint32_t)genet_mac[4] << 8) |
                      ((uint32_t)genet_mac[5]);     /* MAC1 = 16-bit low field */
        GENET_W(UMAC_MAC0, m0);
        GENET_W(UMAC_MAC1, m1);
        for (int i = 0; i < 6; i++) net_mac[i] = genet_mac[i];
    }
#endif

    /* Set max frame length */
    GENET_W(UMAC_MAX_FRAME, 1536);

    /* Configure RBUF */
    GENET_W(RBUF_CTRL, RBUF_ALIGN_2B | RBUF_BAD_DIS);

    /* v0.4.154: TBUF size control -- U-Boot/Linux set this in the reset path;
     * AIOS omitted it. Required for the TX buffer engine. */
    GENET_W(RBUF_TBUF_SIZE_CTRL, 1);

    /* Set port mode to external GPHY (BCM54213) */
    GENET_W(SYS_PORT_CTRL, PORT_MODE_EXT_GPHY);

    /* Initialize PHY */
    if (phy_init() != 0) {
        printf("[net] PHY init failed (continuing without link)\n");
    }

    /* v0.4.155: configure the GENET<->BCM54213 RGMII link (EXT_RGMII_OOB_CTRL).
     * AIOS never did this; U-Boot/Linux set it in adjust_link. This is the lead
     * suspect for the v0.4.154 RX-dead/TX-works split -- RX is the RGMII timing
     * -sensitive direction. Clear OOB_DISABLE, force RGMII_LINK + RGMII_MODE_EN;
     * set ID_MODE_DIS because RPi4 genet is rgmii-rxid (PHY adds the RX delay, so
     * the MAC must not add its own). */
    {
        uint32_t oob = GENET_R(EXT_RGMII_OOB_CTRL);
        oob &= ~OOB_DISABLE;
        oob |= RGMII_LINK | RGMII_MODE_EN | ID_MODE_DIS;
        GENET_W(EXT_RGMII_OOB_CTRL, oob);
    }

#ifndef NETD_BUILD
    /* Allocate DMA buffers (flag-OFF monolithic; flag-ON root does this in
     * plat_net_prov() and netd receives the region via plat_net_dev_attach). */
    if (dma_init() != 0) {
        printf("[net] DMA init failed\n");
        return -1;
    }

    /* v0.4.158: now that the DMA buffer exists, read the REAL board MAC from the
     * VC firmware mailbox and override the fake fallback (so DHCP uses the real
     * MAC). Falls back silently to whatever read_mac_from_umac set. */
    if (read_mac_from_mailbox() == 0)
        printf("[net] real MAC (mailbox): %02x:%02x:%02x:%02x:%02x:%02x\n",
               net_mac[0], net_mac[1], net_mac[2], net_mac[3], net_mac[4], net_mac[5]);
    else
        printf("[net] mailbox MAC read failed, keeping fallback\n");
#endif

    /* Set up descriptor rings */
    ring_init();

#ifndef NETD_BUILD
    /* RX IRQ notification + kick + handler bind (root/vka). netd inherits the
     * handler via plat_net_dev_attach() and self-binds the ntfn to its own TCB. */
    if (genet_irq_bind() != 0) return -1;
#endif

    /* Enable UniMAC TX and RX */
    GENET_W(UMAC_CMD, CMD_TX_EN | CMD_RX_EN | CMD_SPEED_100 |
                       CMD_PAD_EN | CMD_CRC_FWD);
    arch_dsb();

    /* v0.4.155 diag: confirm the RX-side config actually stuck. Expect
     * OOB with RGMII_LINK|RGMII_MODE_EN|ID_MODE_DIS set + OOB_DISABLE clear,
     * RDMActrl/TDMActrl = 0x20001 (DMA_EN | ring16 buf-en), RDMAcfg = 0x10000. */
    arch_dmb();
    printf("[net] cfg: OOB=0x%x RDMActrl=0x%x RDMAcfg=0x%x TDMActrl=0x%x CMD=0x%x\n",
           GENET_R(EXT_RGMII_OOB_CTRL),
           GENET_R(RDMA_CTRL_BASE + DMA_CTRL),
           GENET_R(RDMA_CTRL_BASE + DMA_RING_CFG),
           GENET_R(TDMA_CTRL_BASE + DMA_CTRL),
           GENET_R(UMAC_CMD));

    /* v0.4.162: RX is IRQ-driven by default. Unmask GENET interrupts so RX-done
     * wakes the driver (which blocks on the bound notification). The driver
     * drains-then-rechecks before blocking, so the boot-time DHCP OFFER is still
     * delivered via the IRQ path without a stall. /proc/genet.irqoff reverts. */
    GENET_W(INTRL2_MASK_CLEAR, 0xFFFFFFFFu);
    if (genet_irq_handler) seL4_IRQHandler_Ack(genet_irq_handler);
    arch_dsb();

    net_available = 1;
    genet_initialized = 1;

    printf("[net] RPi4 GENET ready, MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
           net_mac[0], net_mac[1], net_mac[2],
           net_mac[3], net_mac[4], net_mac[5]);
    return 0;
}

/* ================================================================
 * plat_net_tx -- transmit Ethernet frame via GENET TX ring
 * ================================================================ */
int plat_net_tx(const uint8_t *frame, uint32_t len) {
    if (!genet_initialized || len > GENET_PKT_BUF_SIZE || len == 0)
        return -1;

    /* Check for a free descriptor (prod - cons < ring size). */
    arch_dmb();
    uint16_t cons = (uint16_t)(GENET_R(TDMA_RING_BASE + TDMA_CONS_INDEX) & 0xFFFF);
    if ((uint16_t)(tx_prod_idx - cons) >= GENET_TX_DESCS) {
        printf("[net] TX ring full\n");
        return -1;
    }

    uint16_t idx = tx_prod_idx % GENET_TX_DESCS;

    /* Copy frame into the (non-cacheable) DMA buffer. */
    uint8_t *buf = genet_dma + GENET_TX_BUF_OFF +
                   (uint32_t)idx * GENET_PKT_BUF_SIZE;
    memcpy(buf, frame, len);

    /* Write the TX descriptor (in the register block at GENET_TX_DESC_BASE). */
    volatile struct genet_desc *tx_descs =
        (volatile struct genet_desc *)((uintptr_t)genet_regs + GENET_TX_DESC_BASE);

    uint64_t buf_pa = genet_dma_pa + GENET_TX_BUF_OFF +
                      (uint64_t)idx * GENET_PKT_BUF_SIZE;
    tx_descs[idx].addr_lo = (uint32_t)buf_pa;
    tx_descs[idx].addr_hi = (uint32_t)(buf_pa >> 32);
    /* length + QTAG(0x3F) + append-CRC + SOP + EOP; HW takes it on PROD bump. */
    tx_descs[idx].length_status = (len << DESC_LEN_SHIFT) |
                                   (DESC_TX_QTAG_MASK << DESC_TX_QTAG_SHIFT) |
                                   DESC_TX_CRC | DESC_SOP | DESC_EOP;
    arch_dsb();

    /* Advance the producer index (tells HW to transmit). */
    tx_prod_idx++;
    GENET_W(TDMA_RING_BASE + TDMA_PROD_INDEX, tx_prod_idx);

    return 0;
}

/* ================================================================
 * plat_net_drain -- drain the GENET RX ring into net_rx_ring (SPSC).
 *
 * v0.4.230 (netd Stage 1): this is the old driver-thread body, minus the
 * seL4_Wait/Yield (the merged net_server's bound notification wakes its Recv)
 * and the cross-thread Signal (net_server processes net_rx_ring right after).
 * The NAPI re-check is now the do/while condition: after clearing INTRL2 and
 * acking the seL4 IRQ, re-read RDMA_PROD_INDEX and re-drain until producer ==
 * consumer, so a frame that completed during the ack window (its completion IRQ
 * cleared) is consumed here instead of deadlocking the ring. seL4 notifications
 * latch, so a frame arriving after this loop still wakes net_server's Recv --
 * no lost wakeup. The MMIO register sequence is unchanged from the driver
 * thread (HW-verified); only the blocking/signalling around it moved.
 * ================================================================ */
void plat_net_drain(void) {
    volatile struct genet_desc *rx_descs =
        (volatile struct genet_desc *)((uintptr_t)genet_regs + GENET_RX_DESC_BASE);

    /* Bring-up diagnostic state -- persists across calls (was thread-local). */
    static uint16_t last_rxp = 0xFFFF, last_txc = 0xFFFF, last_txp = 0xFFFF;
    static int diag = 0;

    do {
        arch_dmb();
        uint16_t hw_prod = (uint16_t)(GENET_R(RDMA_RING_BASE + RDMA_PROD_INDEX) & 0xFFFF);
        uint16_t tx_cons = (uint16_t)(GENET_R(TDMA_RING_BASE + TDMA_CONS_INDEX) & 0xFFFF);

        /* Diagnostic: report the first index changes so we can tell whether TX
         * frames are consumed by HW (TXc -> TXp = sent) and whether RX frames
         * arrive (RXp advances). Short fields survive console interleaving. */
        if (diag < 24 && (hw_prod != last_rxp || tx_cons != last_txc ||
                          tx_prod_idx != last_txp)) {
            printf("[net-drv] RXp=%u RXc=%u TXp=%u TXc=%u\n",
                   hw_prod, rx_cons_idx, tx_prod_idx, tx_cons);
            last_rxp = hw_prod; last_txc = tx_cons; last_txp = tx_prod_idx; diag++;
        }

        uint16_t cons_before = rx_cons_idx;

        while (rx_cons_idx != hw_prod) {
            uint16_t idx = rx_cons_idx % GENET_RX_DESCS;

            arch_dmb();
            uint32_t ls = rx_descs[idx].length_status;
            uint32_t frame_len = (ls & DESC_LEN_MASK) >> DESC_LEN_SHIFT;

            /* Bad frames are dropped by HW (RBUF_BAD_DIS). The reported length
             * includes the 2-byte RBUF alignment pad and the 4-byte CRC. */
            if (frame_len > 6) {
                frame_len -= 4;  /* CRC */
                uint8_t *src = genet_dma + GENET_RX_BUF_OFF +
                               (uint32_t)idx * GENET_PKT_BUF_SIZE + 2;
                uint32_t pkt_len = frame_len - 2;  /* alignment padding */

                if (pkt_len <= NET_RX_PKT_MAX) {
                    uint32_t h = net_rx_ring.head;
                    uint32_t t = net_rx_ring.tail;
                    if ((h - t) < NET_RX_RING_SIZE) {
                        struct rx_pkt_entry *entry =
                            &net_rx_ring.pkts[h % NET_RX_RING_SIZE];
                        memcpy(entry->data, src, pkt_len);
                        entry->len = (uint16_t)pkt_len;
                        __asm__ volatile("dmb sy" ::: "memory");
                        net_rx_ring.head = h + 1;
                    } else {
                        /* v0.4.225: count the silent drop -- a frozen
                         * consumer (stall quantum) backs the ring up fast */
                        net_rx_stats.ring_overflow_drops++;
                    }
                }
            }

            /* Index model: the descriptor's buffer address persists across
             * laps, so recycling is just advancing the consumer -- no re-arm. */
            rx_cons_idx++;
        }

        /* Update consumer index only when we actually advanced. */
        if (rx_cons_idx != cons_before)
            GENET_W(RDMA_RING_BASE + RDMA_CONS_INDEX, rx_cons_idx);

        /* v0.4.157/159: handle the GENET interrupt. Count + clear asserted bits
         * and re-arm the seL4 IRQ. INTRL2 is unmasked at init, so in steady
         * state ist carries the RX-done bit; .irqoff masks it. */
        {
            uint32_t ist = GENET_R(INTRL2_STAT);
            if (ist) {
                genet_last_intstat = ist;
                GENET_W(INTRL2_CLEAR, ist);
                genet_irq_count++;
            }
            if (genet_irq_handler)
                seL4_IRQHandler_Ack(genet_irq_handler);
        }

        /* NAPI-style re-check (DESIGN_NETD s2): re-read the producer after the
         * ack; if it advanced during the clear window, loop and re-drain. */
        arch_dmb();
    } while ((uint16_t)(GENET_R(RDMA_RING_BASE + RDMA_PROD_INDEX) & 0xFFFF) != rx_cons_idx);
}

/* ================================================================
 * plat_net_get_mac -- return hardware MAC address
 * ================================================================ */
void plat_net_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = genet_mac[i];
}

#endif /* !NETD_PROV (dev half: plat_net_init/tx/drain/get_mac) */

#ifndef NETD_BUILD   /* root-local /proc/genet diag: uses the VC mailbox + root globals */
/* ================================================================
 * Live diagnostic interface -- driven from /proc/genet so the GENET
 * datapath can be probed AND poked from the AIOS shell WITHOUT reflashing.
 * Each `cat` triggers exactly one read (one FS_CAT), so commands run once.
 *
 *   cat /proc/genet                 full register / ring / PHY snapshot
 *   cat /proc/genet.peek.OFF        read MMIO reg at byte offset OFF
 *   cat /proc/genet.poke.OFF.VAL    write VAL to reg OFF (+ readback)
 *   cat /proc/genet.mr.PHY.REG      MDIO read PHY register
 *   cat /proc/genet.mw.PHY.REG.VAL  MDIO write PHY register (+ readback)
 *   cat /proc/genet.tx              send one broadcast test frame
 *   cat /proc/genet.reinit          re-run ring_init (re-apply DMA setup)
 *
 * All numbers hex. Wired into src/procfs.c (PLAT_RPI4). poke/tx/reinit
 * race the net driver/stack -- fine for interactive bring-up, not prod.
 * ================================================================ */
static int diag_pfx(const char **pp, const char *s) {
    const char *p = *pp;
    while (*s) { if (*p != *s) return 0; p++; s++; }
    *pp = p;
    return 1;
}

static uint32_t diag_hex(const char **pp) {
    const char *p = *pp;
    uint32_t v = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    for (;;) {
        char c = *p; uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        v = v * 16u + d; p++;
    }
    if (*p == '.') p++;   /* consume separator */
    *pp = p;
    return v;
}

#ifdef AIOS_NETD
/* Stage 4 (DESIGN_NETD s6): in the flag-ON build root does NOT drive GENET --
 * netd owns the device, so genet_regs is NULL in root. But root keeps its own
 * GENET MMIO mapping at dev_genet_vaddr (claimed in prealloc_rpi4_devices -- the
 * same frames it copied into netd at spawn). /proc/genet renders a READ-ONLY,
 * dead-netd-safe HW view from it: SYS / EXT / RBUF / INTRL2 / RDMA / TDMA ctrl +
 * ring + the RX descriptor RAM. It NEVER touches UMAC or MDIO / PHY -- a UMAC
 * access while SWINIT is latched bus-errors (kernel halt, v0.4.151), and root
 * must not race the live MDIO engine in netd. Software counters, MAC and IP
 * render from /proc/net (the netd stats page); the active poke / tx / mdio ops
 * moved to /bin/netdiag -> NET_DIAG. */
#define GRD(off)       (dev_genet_vaddr[(off) / 4])  /* read-only, root mapping */
#define GENET_UMAC_LO  0x0800u                        /* UMAC core .. */
#define GENET_UMAC_HI  0x0FFFu                        /* .. incl MDIO at 0xE14 */

static int genet_diag_readonly(const char *args, char *buf, int bufsize) {
    if (!dev_genet_vaddr)
        return snprintf(buf, bufsize, "GENET not mapped in root\n");
    arch_dmb();

    if (args[0] == '.') {
        const char *p = args + 1;
        if (diag_pfx(&p, "peek.")) {
            uint32_t off = diag_hex(&p) & ~3u;
            if (off >= 0x10000u)
                return snprintf(buf, bufsize, "off %05x out of range\n", off);
            if (off >= GENET_UMAC_LO && off <= GENET_UMAC_HI)
                return snprintf(buf, bufsize,
                    "off %05x is UMAC/MDIO -- blocked (use netdiag mr/mac)\n", off);
            return snprintf(buf, bufsize, "[%05x] = %08x\n", off, GRD(off));
        }
        return snprintf(buf, bufsize,
            "root /proc/genet is read-only + UMAC/MDIO-free.  cmd: .peek.OFF\n"
            "active ops moved to: netdiag {peek|poke|mr|mw|tx|reinit|irqon|irqoff|mac}\n");
    }

    int w = 0;
    w += snprintf(buf + w, bufsize - w,
        "GENET read-only HW view (root maps it; netd drives). .peek.OFF for more.\n");
    w += snprintf(buf + w, bufsize - w,
        "SYS  rev=%08x port=%x rbufflush=%x tbufflush=%x  EXT oob(8c)=%x  RBUF ctrl(300)=%x\n",
        GRD(SYS_REV_CTRL), GRD(SYS_PORT_CTRL), GRD(SYS_RBUF_FLUSH_CTRL),
        GRD(SYS_TBUF_FLUSH_CTRL), GRD(EXT_RGMII_OOB_CTRL), GRD(RBUF_CTRL));
    w += snprintf(buf + w, bufsize - w,
        "INTRL2 stat(200)=%x maskstat(20c)=%x\n",
        GRD(INTRL2_STAT), GRD(INTRL2_MASK_STATUS));
    w += snprintf(buf + w, bufsize - w,
        "RDMA ctrl(3044)=%x cfg(3040)=%x scb(304c)=%x  prod(08)=%x cons(0c)=%x start=%x end=%x bufsz=%x\n",
        GRD(RDMA_CTRL_BASE + DMA_CTRL), GRD(RDMA_CTRL_BASE + DMA_RING_CFG),
        GRD(RDMA_CTRL_BASE + DMA_SCB_BURST_SIZE),
        GRD(RDMA_RING_BASE + RDMA_PROD_INDEX), GRD(RDMA_RING_BASE + RDMA_CONS_INDEX),
        GRD(RDMA_RING_BASE + DMA_START_ADDR), GRD(RDMA_RING_BASE + DMA_END_ADDR),
        GRD(RDMA_RING_BASE + DMA_RING_BUF_SIZE));
    w += snprintf(buf + w, bufsize - w,
        "TDMA ctrl(5044)=%x cfg(5040)=%x  prod(0c)=%x cons(08)=%x\n",
        GRD(TDMA_CTRL_BASE + DMA_CTRL), GRD(TDMA_CTRL_BASE + DMA_RING_CFG),
        GRD(TDMA_RING_BASE + TDMA_PROD_INDEX), GRD(TDMA_RING_BASE + TDMA_CONS_INDEX));
    w += snprintf(buf + w, bufsize - w, "RXdesc.ls:");
    volatile struct genet_desc *rd =
        (volatile struct genet_desc *)((uintptr_t)dev_genet_vaddr + GENET_RX_DESC_BASE);
    for (int i = 0; i < 8 && i < GENET_RX_DESCS; i++)
        w += snprintf(buf + w, bufsize - w, " %x", rd[i].length_status);
    w += snprintf(buf + w, bufsize - w,
        "\n(MAC / IP / irq counters / DHCP state -> /proc/net)\n");
    return w;
}
#undef GRD
#undef GENET_UMAC_LO
#undef GENET_UMAC_HI
#endif /* AIOS_NETD */

#ifndef AIOS_NETD   /* active diag helpers below: flag-OFF only (root drives GENET) */
static uint32_t diag_peek(uint32_t off) {
    if (off >= 0x10000) return 0xDEADBEEFu;
    arch_dmb();
    return GENET_R(off & ~3u);
}

static uint32_t diag_poke(uint32_t off, uint32_t val) {
    if (off >= 0x10000) return 0xDEADBEEFu;
    GENET_W(off & ~3u, val);
    arch_dmb();
    return GENET_R(off & ~3u);
}

static int diag_tx_test(void) {
    /* 60-byte broadcast ARP request (sender = our MAC) to exercise TX. */
    uint8_t f[60];
    memset(f, 0, sizeof(f));
    for (int i = 0; i < 6; i++) { f[i] = 0xFF; f[6 + i] = genet_mac[i]; }
    f[12] = 0x08; f[13] = 0x06;                       /* ethertype ARP */
    f[14] = 0x00; f[15] = 0x01;                       /* HW type ethernet */
    f[16] = 0x08; f[17] = 0x00;                       /* proto IPv4 */
    f[18] = 6; f[19] = 4; f[20] = 0x00; f[21] = 0x01; /* request */
    for (int i = 0; i < 6; i++) f[22 + i] = genet_mac[i];
    return plat_net_tx(f, sizeof(f));
}

static int diag_dump(char *buf, int bufsize) {
    arch_dmb();
    int w = 0;
    uint16_t bmcr = mdio_read(PHY_ADDR, MII_BMCR);
    uint16_t bmsr = mdio_read(PHY_ADDR, MII_BMSR);
    w += snprintf(buf + w, bufsize - w,
        "GENET diag (hex). cmds: .peek.OFF .poke.OFF.VAL .mr.P.R .mw.P.R.V .tx .reinit\n");
    w += snprintf(buf + w, bufsize - w,
        "SYS  rev=%08x port=%x rbufflush=%x  EXT oob(8c)=%x  RBUF ctrl(300)=%x\n",
        GENET_R(SYS_REV_CTRL), GENET_R(SYS_PORT_CTRL),
        GENET_R(SYS_RBUF_FLUSH_CTRL), GENET_R(EXT_RGMII_OOB_CTRL), GENET_R(RBUF_CTRL));
    w += snprintf(buf + w, bufsize - w,
        "UMAC cmd(808)=%x mac=%02x%02x%02x%02x%02x%02x\n",
        GENET_R(UMAC_CMD), genet_mac[0], genet_mac[1], genet_mac[2],
        genet_mac[3], genet_mac[4], genet_mac[5]);
    w += snprintf(buf + w, bufsize - w,
        "RDMA ctrl(3044)=%x cfg(3040)=%x scb(304c)=%x  prod(08)=%x cons(0c)=%x start=%x end=%x bufsz=%x\n",
        GENET_R(RDMA_CTRL_BASE + DMA_CTRL), GENET_R(RDMA_CTRL_BASE + DMA_RING_CFG),
        GENET_R(RDMA_CTRL_BASE + DMA_SCB_BURST_SIZE),
        GENET_R(RDMA_RING_BASE + RDMA_PROD_INDEX), GENET_R(RDMA_RING_BASE + RDMA_CONS_INDEX),
        GENET_R(RDMA_RING_BASE + DMA_START_ADDR), GENET_R(RDMA_RING_BASE + DMA_END_ADDR),
        GENET_R(RDMA_RING_BASE + DMA_RING_BUF_SIZE));
    w += snprintf(buf + w, bufsize - w,
        "TDMA ctrl(5044)=%x cfg(5040)=%x  prod(0c)=%x cons(08)=%x\n",
        GENET_R(TDMA_CTRL_BASE + DMA_CTRL), GENET_R(TDMA_CTRL_BASE + DMA_RING_CFG),
        GENET_R(TDMA_RING_BASE + TDMA_PROD_INDEX), GENET_R(TDMA_RING_BASE + TDMA_CONS_INDEX));
    w += snprintf(buf + w, bufsize - w,
        "SW   rxc=%x txp=%x net_avail=%d  RXdesc.ls:",
        rx_cons_idx, tx_prod_idx, net_available);
    volatile struct genet_desc *rd =
        (volatile struct genet_desc *)((uintptr_t)genet_regs + GENET_RX_DESC_BASE);
    for (int i = 0; i < 8 && i < GENET_RX_DESCS; i++)
        w += snprintf(buf + w, bufsize - w, " %x", rd[i].length_status);
    w += snprintf(buf + w, bufsize - w,
        "\nPHY  bmcr=%x bmsr=%x link=%d\n", bmcr, bmsr, (bmsr & BMSR_LINK) ? 1 : 0);
    w += snprintf(buf + w, bufsize - w,
        "IRQ  count=%x laststat=%x mask(20c)=%x  (.mac reads real MAC; unmask IRQ: poke.214.<bits>)\n",
        genet_irq_count, genet_last_intstat, GENET_R(INTRL2_MASK_STATUS));
    return w;
}
#endif /* !AIOS_NETD (active diag helpers) */

int genet_diag_cmd(const char *args, char *buf, int bufsize) {
#ifdef AIOS_NETD
    /* flag-ON: root is prov-only; render the read-only, UMAC/MDIO-free view. */
    return genet_diag_readonly(args, buf, bufsize);
#else
    if (!genet_regs)
        return snprintf(buf, bufsize, "GENET not present/initialized\n");
    if (args[0] == '\0')
        return diag_dump(buf, bufsize);
    if (args[0] != '.')
        return -1;
    const char *p = args + 1;

    if (diag_pfx(&p, "peek.")) {
        uint32_t off = diag_hex(&p);
        return snprintf(buf, bufsize, "[%05x] = %08x\n", off, diag_peek(off));
    }
    if (diag_pfx(&p, "poke.")) {
        uint32_t off = diag_hex(&p);
        uint32_t val = diag_hex(&p);
        uint32_t rb = diag_poke(off, val);
        return snprintf(buf, bufsize, "[%05x] <= %08x  readback %08x\n", off, val, rb);
    }
    if (diag_pfx(&p, "mr.")) {
        int phy = (int)diag_hex(&p);
        int reg = (int)diag_hex(&p);
        return snprintf(buf, bufsize, "mdio phy %x reg %x = %04x\n",
                        phy, reg, mdio_read(phy & 0x1f, reg & 0x1f));
    }
    if (diag_pfx(&p, "mw.")) {
        int phy = (int)diag_hex(&p);
        int reg = (int)diag_hex(&p);
        uint16_t val = (uint16_t)diag_hex(&p);
        mdio_write(phy & 0x1f, reg & 0x1f, val);
        return snprintf(buf, bufsize, "mdio phy %x reg %x <= %04x  readback %04x\n",
                        phy, reg, val, mdio_read(phy & 0x1f, reg & 0x1f));
    }
    if (diag_pfx(&p, "tx")) {
        int r = diag_tx_test();
        arch_dmb();
        return snprintf(buf, bufsize, "tx ret=%d  txp=%x txc=%x\n", r, tx_prod_idx,
                        (uint16_t)(GENET_R(TDMA_RING_BASE + TDMA_CONS_INDEX) & 0xFFFF));
    }
    if (diag_pfx(&p, "reinit")) {
        ring_init();
        return snprintf(buf, bufsize, "ring_init re-run\n");
    }
    if (diag_pfx(&p, "mac")) {
        int r = read_mac_from_mailbox();
        return snprintf(buf, bufsize,
            "mac read ret=%d -> %02x:%02x:%02x:%02x:%02x:%02x\n", r,
            genet_mac[0], genet_mac[1], genet_mac[2],
            genet_mac[3], genet_mac[4], genet_mac[5]);
    }
    if (diag_pfx(&p, "ip")) {
        /* One short line (survives the lossy mini-UART): MAC, net config, RX
         * producer index, IRQ count, and the DHCP failure-mode counters. After a
         * boot: bnd=1 + a real ip=... means a lease; bnd=0 + rep=0 means no DHCP
         * replies reached us; off=0/ack=0/mis>0 localize the rest. */
        arch_dmb();
        uint16_t rxp = (uint16_t)(GENET_R(RDMA_RING_BASE + RDMA_PROD_INDEX) & 0xFFFF);
        return snprintf(buf, bufsize,
            "mac=%02x%02x%02x%02x%02x%02x ip=%d.%d.%d.%d gw=%d.%d.%d.%d bnd=%d "
            "rxp=%x irq=%x rep=%d off=%d ack=%d mis=%d\n",
            genet_mac[0], genet_mac[1], genet_mac[2],
            genet_mac[3], genet_mac[4], genet_mac[5],
            net_cfg_ip[0], net_cfg_ip[1], net_cfg_ip[2], net_cfg_ip[3],
            net_cfg_gw[0], net_cfg_gw[1], net_cfg_gw[2], net_cfg_gw[3],
            dhcp_bound, rxp, genet_irq_count,
            dhcp_replies, dhcp_offers, dhcp_acks, dhcp_mismatch);
    }
    if (diag_pfx(&p, "irqon")) {
        GENET_W(INTRL2_MASK_CLEAR, 0xFFFFFFFFu);   /* unmask all GENET interrupts */
        if (genet_irq_handler) seL4_IRQHandler_Ack(genet_irq_handler);
        net_rx_irq_mode = 1;
        return snprintf(buf, bufsize,
            "RX IRQ-driven ON: INTRL2 unmasked, driver blocks on IRQ. "
            "Check .ip rxp keeps climbing; .irqoff reverts.\n");
    }
    if (diag_pfx(&p, "irqoff")) {
        GENET_W(INTRL2_MASK_SET, 0xFFFFFFFFu);     /* mask all GENET interrupts */
        net_rx_irq_mode = 0;
        /* v0.4.230 (Stage 1): kick the merged net_server via the badge=2 copy
         * of its bound notification -- there is no separate driver thread. With
         * INTRL2 masked, RX no longer auto-wakes; .irqon reverts. */
        if (net_kick_ntfn_cap) seL4_Signal(net_kick_ntfn_cap);
        return snprintf(buf, bufsize, "RX IRQ masked; net_server kicked (badge 2).\n");
    }
    return -1;
#endif /* AIOS_NETD */
}

#endif /* !NETD_BUILD (root-local /proc/genet diag) */

#ifdef NETD_BUILD
/* netd Stage 4 (DESIGN_NETD s6): serialized live-device diagnostics for the
 * userland /bin/netdiag tool. These touch the device netd owns (poke / MDIO / tx /
 * irq), so they live HERE (netd), not in root /proc/genet. net_server dispatches
 * NET_DIAG ops here; op codes in netd_ctrl.h. Reply: return = status, out[] =
 * result words. This RACES the live net stack -- fine for interactive bring-up. */
int plat_net_diag(int op, uint32_t a, uint32_t b, uint32_t c, uint32_t out[2]) {
    out[0] = 0; out[1] = 0;
    if (!genet_regs) return -1;
    switch (op) {
    case NETD_DIAG_PEEK: {
        uint32_t off = a & ~3u;
        if (off >= 0x10000u) return -1;
        arch_dmb();
        out[0] = GENET_R(off);
        return 0;
    }
    case NETD_DIAG_POKE: {
        uint32_t off = a & ~3u;
        if (off >= 0x10000u) return -1;
        GENET_W(off, b);
        arch_dmb();
        out[0] = GENET_R(off);
        return 0;
    }
    case NETD_DIAG_MR:
        out[0] = mdio_read((int)(a & 0x1f), (int)(b & 0x1f));
        return 0;
    case NETD_DIAG_MW:
        mdio_write((int)(a & 0x1f), (int)(b & 0x1f), (uint16_t)c);
        out[0] = mdio_read((int)(a & 0x1f), (int)(b & 0x1f));
        return 0;
    case NETD_DIAG_TX: {
        /* 60-byte broadcast ARP request (sender = our MAC) to exercise TX. */
        uint8_t f[60];
        memset(f, 0, sizeof(f));
        for (int i = 0; i < 6; i++) { f[i] = 0xFF; f[6 + i] = genet_mac[i]; }
        f[12] = 0x08; f[13] = 0x06; f[14] = 0x00; f[15] = 0x01;
        f[16] = 0x08; f[17] = 0x00; f[18] = 6; f[19] = 4; f[21] = 0x01;
        for (int i = 0; i < 6; i++) f[22 + i] = genet_mac[i];
        int r = plat_net_tx(f, sizeof(f));
        arch_dmb();
        out[0] = tx_prod_idx;
        out[1] = (uint16_t)(GENET_R(TDMA_RING_BASE + TDMA_CONS_INDEX) & 0xFFFF);
        return r;
    }
    case NETD_DIAG_REINIT:
        ring_init();
        return 0;
    case NETD_DIAG_IRQON:
        GENET_W(INTRL2_MASK_CLEAR, 0xFFFFFFFFu);   /* unmask all GENET interrupts */
        if (genet_irq_handler) seL4_IRQHandler_Ack(genet_irq_handler);
        net_rx_irq_mode = 1;
        return 0;
    case NETD_DIAG_IRQOFF:
        GENET_W(INTRL2_MASK_SET, 0xFFFFFFFFu);     /* mask all GENET interrupts */
        net_rx_irq_mode = 0;
        if (net_kick_ntfn_cap) seL4_Signal(net_kick_ntfn_cap);
        return 0;
    case NETD_DIAG_MAC:
        /* The MAC netd latched at attach (argv from the root mailbox read). */
        out[0] = ((uint32_t)genet_mac[0] << 24) | ((uint32_t)genet_mac[1] << 16) |
                 ((uint32_t)genet_mac[2] <<  8) |  (uint32_t)genet_mac[3];
        out[1] = ((uint32_t)genet_mac[4] <<  8) |  (uint32_t)genet_mac[5];
        return 0;
    default:
        return -1;
    }
}
#endif /* NETD_BUILD */

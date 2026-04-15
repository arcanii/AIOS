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
#include "aios/vka_audit.h"
#include <sel4platsupport/device.h>
#include <stdio.h>
#include <string.h>
#include "arch.h"
#include "aios/hw_info.h"
#include "plat/net_hal.h"

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

/* TBUF (transmit buffer control) */
#define TBUF_CTRL           0x0600

/* UniMAC registers (offset 0x0800) */
#define UMAC_BASE           0x0800
#define UMAC_CMD            (UMAC_BASE + 0x008)
#define UMAC_MAC0           (UMAC_BASE + 0x00C)  /* MAC[5:2] */
#define UMAC_MAC1           (UMAC_BASE + 0x010)  /* MAC[1:0] */
#define UMAC_MAX_FRAME      (UMAC_BASE + 0x014)
#define UMAC_MDIO_CMD       (UMAC_BASE + 0x614)

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

/* GENET DMA registers (offset 0x2000 for RX, 0x4000 for TX)
 * Each DMA engine has control, status, and per-ring registers.
 * We use default queue 16 (rings 0-15 are priority queues). */

/* DMA control block offsets */
#define RDMA_BASE           0x2000
#define TDMA_BASE           0x4000

/* DMA ring config: ring 16 = default ring.
 * Ring 16 registers are at DMA_BASE + 0x10 * RING + offset.
 * But the default ring (DESC_INDEX=16) has special register layout. */
#define DMA_RINGS           0x0200   /* offset to ring registers from DMA_BASE */
#define DMA_RING_SIZE       0x40     /* bytes per ring register set */

/* DMA control register (at DMA_BASE + 0x00) */
#define DMA_CTRL            0x00
#define DMA_CTRL_EN         (1u << 0)

/* DMA status register */
#define DMA_STATUS          0x04

/* DMA SCB burst size */
#define DMA_SCB_BURST       0x0C

/* Ring 16 (default) register offsets from DMA_BASE + DMA_RINGS + 16*DMA_RING_SIZE */
#define DMA_RING16_OFF      (DMA_RINGS + 16 * DMA_RING_SIZE)

/* Per-ring registers (offset from ring base) */
#define RING_READ_PTR       0x00
#define RING_READ_PTR_HI    0x04
#define RING_CONS_INDEX     0x08
#define RING_PROD_INDEX     0x0C
#define RING_BUF_SIZE       0x10     /* (buf_len << 16) | ring_size */
#define RING_START_ADDR     0x14
#define RING_START_ADDR_HI  0x18
#define RING_END_ADDR       0x1C
#define RING_END_ADDR_HI    0x20
#define RING_MBUF_DONE_THR  0x24
#define RING_XON_XOFF_THR   0x28
#define RING_FLOW_PERIOD    0x2C

/* DMA descriptor format (12 bytes each) */
struct genet_desc {
    uint32_t length_status;
    uint32_t addr_lo;
    uint32_t addr_hi;
};

/* Descriptor length_status bits */
#define DESC_OWN            (1u << 15)  /* owned by HW */
#define DESC_EOP            (1u << 14)  /* end of packet */
#define DESC_SOP            (1u << 13)  /* start of packet */
#define DESC_WRAP           (1u << 12)  /* wrap ring */
#define DESC_CRC            (1u << 11)  /* append CRC (TX) */
#define DESC_LEN_SHIFT      16
#define DESC_LEN_MASK       0xFFF0000u

/* RX descriptor status bits */
#define RDESC_OVFLOW        (1u << 0)
#define RDESC_CRC_ERR       (1u << 1)
#define RDESC_RX_ERR        (1u << 2)
#define RDESC_NO            (1u << 3)
#define RDESC_LG            (1u << 4)   /* frame too long */

/* Descriptor ring in DMA-able memory (at 0x10000 within GENET regs) */
#define GENET_DESC_BASE     0x10000

/* TX descriptors start at GENET_DESC_BASE + 0 */
/* RX descriptors start at GENET_DESC_BASE + 256*12 */
#define GENET_TX_DESC_OFF   0
#define GENET_RX_DESC_OFF   (256 * 12)

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
    for (int t = 0; t < 1000000; t++) {
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

    for (int t = 0; t < 1000000; t++) {
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
    genet_mac[4] = (uint8_t)(mac1 >> 24);
    genet_mac[5] = (uint8_t)(mac1 >> 16);

    /* Copy to global net_mac for stack */
    for (int i = 0; i < 6; i++) net_mac[i] = genet_mac[i];
}

/* ----------------------------------------------------------------
 * dma_init -- set up RX and TX descriptor rings
 * ---------------------------------------------------------------- */
static int dma_init(void) {
    int error;

    /* Allocate 128KB DMA region (size-17 untyped = 32 x 4K pages) */
    vka_object_t dma_ut;
    vka_audit_untyped(VKA_SUB_NET, 17);
    error = vka_alloc_untyped(&vka, 17, &dma_ut);
    if (error) {
        printf("[net] DMA untyped alloc failed: %d\n", error);
        return -1;
    }

    seL4_CPtr dma_caps[GENET_DMA_FRAMES];
    for (int i = 0; i < GENET_DMA_FRAMES; i++) {
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
        dma_caps[i] = slot;
    }

    void *dma_vaddr = vspace_map_pages(&vspace, dma_caps, NULL,
        seL4_AllRights, GENET_DMA_FRAMES, seL4_PageBits, 0);
    if (!dma_vaddr) {
        printf("[net] DMA map failed\n");
        return -1;
    }

    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(dma_caps[0]);
    if (ga.error) {
        printf("[net] DMA GetAddress failed\n");
        return -1;
    }

    genet_dma = (uint8_t *)dma_vaddr;
    genet_dma_pa = ga.paddr;
    memset(genet_dma, 0, GENET_DMA_SIZE);

    printf("[net] DMA region: virt=%p phys=0x%lx (128KB)\n",
           dma_vaddr, (unsigned long)genet_dma_pa);
    return 0;
}

/* ----------------------------------------------------------------
 * ring_init -- configure RX and TX default rings (ring 16)
 * ---------------------------------------------------------------- */
static void ring_init(void) {
    uint32_t rdma_ring = RDMA_BASE + DMA_RING16_OFF;
    uint32_t tdma_ring = TDMA_BASE + DMA_RING16_OFF;

    /* Disable DMA engines first */
    GENET_W(RDMA_BASE + DMA_CTRL, 0);
    GENET_W(TDMA_BASE + DMA_CTRL, 0);
    genet_delay(1000);

    /* --- RX ring 16 (default) --- */
    /* Descriptor addresses use the GENET internal SRAM at 0x10000 */
    uint32_t rx_desc_base = GENET_RX_DESC_OFF;
    uint32_t rx_desc_end  = rx_desc_base + GENET_RX_DESCS * 12 - 1;

    GENET_W(rdma_ring + RING_START_ADDR, rx_desc_base);
    GENET_W(rdma_ring + RING_START_ADDR_HI, 0);
    GENET_W(rdma_ring + RING_END_ADDR, rx_desc_end);
    GENET_W(rdma_ring + RING_END_ADDR_HI, 0);
    GENET_W(rdma_ring + RING_BUF_SIZE,
            (GENET_PKT_BUF_SIZE << 16) | GENET_RX_DESCS);
    GENET_W(rdma_ring + RING_READ_PTR, 0);
    GENET_W(rdma_ring + RING_READ_PTR_HI, 0);
    GENET_W(rdma_ring + RING_CONS_INDEX, 0);
    GENET_W(rdma_ring + RING_PROD_INDEX, 0);
    GENET_W(rdma_ring + RING_MBUF_DONE_THR, 1);
    GENET_W(rdma_ring + RING_XON_XOFF_THR, (5u << 16) | 10u);

    /* Set up RX descriptors pointing to DMA buffers */
    volatile struct genet_desc *rx_descs =
        (volatile struct genet_desc *)((uintptr_t)genet_regs + GENET_DESC_BASE + GENET_RX_DESC_OFF);

    for (int i = 0; i < GENET_RX_DESCS; i++) {
        uint64_t buf_pa = genet_dma_pa + GENET_RX_BUF_OFF +
                          (uint64_t)i * GENET_PKT_BUF_SIZE;
        rx_descs[i].addr_lo = (uint32_t)buf_pa;
        rx_descs[i].addr_hi = (uint32_t)(buf_pa >> 32);
        rx_descs[i].length_status = GENET_PKT_BUF_SIZE << DESC_LEN_SHIFT;
    }
    arch_dsb();

    /* Advance producer index to make all descriptors available */
    rx_prod_idx = GENET_RX_DESCS;
    rx_cons_idx = 0;
    GENET_W(rdma_ring + RING_PROD_INDEX, rx_prod_idx);

    /* --- TX ring 16 (default) --- */
    uint32_t tx_desc_base = GENET_TX_DESC_OFF;
    uint32_t tx_desc_end  = tx_desc_base + GENET_TX_DESCS * 12 - 1;

    GENET_W(tdma_ring + RING_START_ADDR, tx_desc_base);
    GENET_W(tdma_ring + RING_START_ADDR_HI, 0);
    GENET_W(tdma_ring + RING_END_ADDR, tx_desc_end);
    GENET_W(tdma_ring + RING_END_ADDR_HI, 0);
    GENET_W(tdma_ring + RING_BUF_SIZE,
            (GENET_PKT_BUF_SIZE << 16) | GENET_TX_DESCS);
    GENET_W(tdma_ring + RING_READ_PTR, 0);
    GENET_W(tdma_ring + RING_READ_PTR_HI, 0);
    GENET_W(tdma_ring + RING_CONS_INDEX, 0);
    GENET_W(tdma_ring + RING_PROD_INDEX, 0);

    tx_prod_idx = 0;
    tx_cons_idx = 0;

    /* Enable DMA engines */
    GENET_W(RDMA_BASE + DMA_CTRL, DMA_CTRL_EN | (1u << 17));  /* ring 16 enable */
    GENET_W(TDMA_BASE + DMA_CTRL, DMA_CTRL_EN | (1u << 17));  /* ring 16 enable */
    arch_dsb();
}

/* ================================================================
 * plat_net_init -- initialize BCM54213 GENET on RPi4
 * ================================================================ */
int plat_net_init(void) {
    /* GENET disabled until driver is stable on RPi4 */
    printf("[net] GENET disabled (Phase 2 WIP)\n");
    return -1;
    int error;

    if (!hw_info.has_genet) {
        printf("[net] No GENET in DTB\n");
        return -1;
    }

    printf("[net] GENET at 0x%lx IRQ %u\n",
           (unsigned long)hw_info.genet_paddr, hw_info.genet_irq);

    /* Map GENET register region (64KB = 16 pages) */
    seL4_CPtr genet_caps[GENET_NUM_PAGES];
    for (int p = 0; p < GENET_NUM_PAGES; p++) {
        vka_object_t frame;
        error = sel4platsupport_alloc_frame_at(&vka,
            hw_info.genet_paddr + (uint64_t)p * 0x1000,
            seL4_PageBits, &frame);
        if (error) {
            printf("[net] MMIO page %d alloc failed: %d\n", p, error);
            return -1;
        }
        genet_caps[p] = frame.cptr;
    }

    void *genet_vaddr = vspace_map_pages(&vspace, genet_caps, NULL,
        seL4_AllRights, GENET_NUM_PAGES, seL4_PageBits, 0);
    if (!genet_vaddr) {
        printf("[net] MMIO map failed\n");
        return -1;
    }
    genet_regs = (volatile uint32_t *)genet_vaddr;

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

    /* --- UniMAC reset --- */
    GENET_W(UMAC_CMD, CMD_SW_RESET);
    genet_delay(10000);
    GENET_W(UMAC_CMD, 0);
    genet_delay(10000);

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
        uint32_t m1 = ((uint32_t)genet_mac[4] << 24) |
                      ((uint32_t)genet_mac[5] << 16);
        GENET_W(UMAC_MAC0, m0);
        GENET_W(UMAC_MAC1, m1);
        for (int i = 0; i < 6; i++) net_mac[i] = genet_mac[i];
    }

    /* Set max frame length */
    GENET_W(UMAC_MAX_FRAME, 1536);

    /* Configure RBUF */
    GENET_W(RBUF_CTRL, RBUF_ALIGN_2B | RBUF_BAD_DIS);

    /* Set port mode to external GPHY (BCM54213) */
    GENET_W(SYS_PORT_CTRL, PORT_MODE_EXT_GPHY);

    /* Initialize PHY */
    if (phy_init() != 0) {
        printf("[net] PHY init failed (continuing without link)\n");
    }

    /* Allocate DMA buffers */
    if (dma_init() != 0) {
        printf("[net] DMA init failed\n");
        return -1;
    }

    /* Set up descriptor rings */
    ring_init();

    /* Allocate notification objects for driver/server IPC */
    vka_object_t drv_ntfn_obj, srv_ntfn_obj;
    error = vka_alloc_notification(&vka, &drv_ntfn_obj);
    if (error) {
        printf("[net] drv notification alloc failed\n");
        return -1;
    }
    error = vka_alloc_notification(&vka, &srv_ntfn_obj);
    if (error) {
        printf("[net] srv notification alloc failed\n");
        return -1;
    }
    net_drv_ntfn_cap = drv_ntfn_obj.cptr;
    net_srv_ntfn_cap = srv_ntfn_obj.cptr;

    /* Bind GENET IRQ to driver notification */
    {
        cspacepath_t irq_path;
        int irq_err = vka_cspace_alloc_path(&vka, &irq_path);
        if (!irq_err) {
            irq_err = simple_get_IRQ_handler(&simple, hw_info.genet_irq,
                                              irq_path);
            if (!irq_err) {
                genet_irq_handler = irq_path.capPtr;
                irq_err = seL4_IRQHandler_SetNotification(
                    genet_irq_handler, net_drv_ntfn_cap);
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

    /* Enable UniMAC TX and RX */
    GENET_W(UMAC_CMD, CMD_TX_EN | CMD_RX_EN | CMD_SPEED_100 |
                       CMD_PAD_EN | CMD_CRC_FWD);
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

    uint32_t tdma_ring = TDMA_BASE + DMA_RING16_OFF;

    /* Check for space in TX ring */
    arch_dmb();
    uint16_t cons = (uint16_t)(GENET_R(tdma_ring + RING_CONS_INDEX) & 0xFFFF);
    if ((uint16_t)(tx_prod_idx - cons) >= GENET_TX_DESCS) {
        printf("[net] TX ring full\n");
        return -1;
    }

    uint16_t idx = tx_prod_idx % GENET_TX_DESCS;

    /* Copy frame to DMA buffer */
    uint8_t *buf = genet_dma + GENET_TX_BUF_OFF +
                   (uint32_t)idx * GENET_PKT_BUF_SIZE;
    memcpy(buf, frame, len);

    /* Write TX descriptor */
    volatile struct genet_desc *tx_descs =
        (volatile struct genet_desc *)((uintptr_t)genet_regs + GENET_DESC_BASE + GENET_TX_DESC_OFF);

    uint64_t buf_pa = genet_dma_pa + GENET_TX_BUF_OFF +
                      (uint64_t)idx * GENET_PKT_BUF_SIZE;
    tx_descs[idx].addr_lo = (uint32_t)buf_pa;
    tx_descs[idx].addr_hi = (uint32_t)(buf_pa >> 32);
    tx_descs[idx].length_status = (len << DESC_LEN_SHIFT) |
                                   DESC_SOP | DESC_EOP | DESC_CRC;
    arch_dsb();

    /* Advance producer index */
    tx_prod_idx++;
    GENET_W(tdma_ring + RING_PROD_INDEX, tx_prod_idx);

    return 0;
}

/* ================================================================
 * plat_net_driver_fn -- RX driver thread (polling + IRQ)
 *
 * Polls the RX ring for completed descriptors, copies frames
 * into the shared net_rx_ring (SPSC) for net_server.
 * ================================================================ */
void plat_net_driver_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr drv_ntfn = (seL4_CPtr)(uintptr_t)arg0;
    (void)arg1; (void)ipc_buf;

    uint32_t rdma_ring = RDMA_BASE + DMA_RING16_OFF;

    volatile struct genet_desc *rx_descs =
        (volatile struct genet_desc *)((uintptr_t)genet_regs + GENET_DESC_BASE + GENET_RX_DESC_OFF);

    printf("[net-drv] GENET driver thread ready\n");

    while (1) {
        seL4_Word badge;
        seL4_Wait(drv_ntfn, &badge);

        int drained = 0;

        /* Read current producer index from hardware */
        arch_dmb();
        uint16_t hw_prod = (uint16_t)(GENET_R(rdma_ring + RING_PROD_INDEX) & 0xFFFF);

        while (rx_cons_idx != hw_prod) {
            uint16_t idx = rx_cons_idx % GENET_RX_DESCS;

            arch_dmb();
            uint32_t ls = rx_descs[idx].length_status;
            uint32_t frame_len = (ls & DESC_LEN_MASK) >> DESC_LEN_SHIFT;

            /* Skip error frames */
            if (ls & (RDESC_OVFLOW | RDESC_CRC_ERR | RDESC_RX_ERR)) {
                rx_cons_idx++;
                continue;
            }

            /* Strip CRC (4 bytes) and 2-byte RBUF alignment padding */
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
                        drained++;
                    }
                }
            }

            /* Recycle descriptor */
            uint64_t buf_pa = genet_dma_pa + GENET_RX_BUF_OFF +
                              (uint64_t)idx * GENET_PKT_BUF_SIZE;
            rx_descs[idx].addr_lo = (uint32_t)buf_pa;
            rx_descs[idx].addr_hi = (uint32_t)(buf_pa >> 32);
            rx_descs[idx].length_status = GENET_PKT_BUF_SIZE << DESC_LEN_SHIFT;
            arch_dsb();

            rx_cons_idx++;
        }

        /* Update consumer index so HW knows we processed these */
        GENET_W(rdma_ring + RING_CONS_INDEX, rx_cons_idx);

        if (drained > 0) {
            seL4_Signal(net_srv_ntfn_cap);
        }

        /* ACK IRQ */
        if (genet_irq_handler)
            seL4_IRQHandler_Ack(genet_irq_handler);
    }
}

/* ================================================================
 * plat_net_get_mac -- return hardware MAC address
 * ================================================================ */
void plat_net_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = genet_mac[i];
}

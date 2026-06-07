/*
 * xhci.c -- xHCI host controller driver (shared, platform-independent)
 *
 * USB HID arc Layer 2 (docs/DESIGN_USB_HID.md). Phase B: map the register space
 * from the BAR the PCIe backend programmed (pcie_xhci_bar), reset the controller,
 * set up the DCBAA / command ring / event ring, run it, and detect connected
 * ports. Phase C (enumeration + HID) builds on the rings established here.
 *
 * DMA: each control structure fits in one 4 KB page, so each is a single frame
 * mapped NON-CACHEABLE (the controller reads them via DMA; non-cacheable avoids
 * manual cache maintenance and is correct on the A72 -- QEMU does not model the
 * cache, so this is also the only HW-safe choice here). The controller is handed
 * PHYSICAL addresses (seL4_ARM_Page_GetAddress), never vaddrs. Poll waits are
 * time-bounded via the generic timer (mono_wait.h), never iteration counts.
 */
#include "aios/root_shared.h"
#include <sel4/sel4.h>
#include <vka/object.h>
#include <sel4platsupport/device.h>
#include <stdio.h>
#include "arch.h"
#include "aios/mono_wait.h"
#include "aios/pcie.h"
#include "aios/xhci.h"
#define LOG_MODULE "xhci"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "aios/aios_log.h"

/* ---- Capability registers (BAR + 0) ---- */
#define XHCI_CAPLENGTH    0x00  /* u8  */
#define XHCI_HCIVERSION   0x02  /* u16 */
#define XHCI_HCSPARAMS1   0x04  /* [31:24]=MaxPorts [18:8]=MaxIntrs [7:0]=MaxSlots */
#define XHCI_HCSPARAMS2   0x08  /* scratchpad-buffer count fields */
#define XHCI_HCCPARAMS1   0x10  /* [0]=AC64 [2]=CSZ ... */
#define XHCI_DBOFF        0x14  /* doorbell array offset (dword-aligned) */
#define XHCI_RTSOFF       0x18  /* runtime register space offset */

/* ---- Operational registers (BAR + CAPLENGTH) ---- */
#define XHCI_USBCMD       0x00
#define XHCI_USBSTS       0x04
#define XHCI_PAGESIZE     0x08
#define XHCI_CRCR         0x18  /* u64 -- command ring control */
#define XHCI_DCBAAP       0x30  /* u64 -- device context base addr array ptr */
#define XHCI_CONFIG       0x38
#define XHCI_PORTSC(n)    (0x400 + (n) * 0x10)  /* n = 0-based port index */

/* USBCMD / USBSTS bits */
#define USBCMD_RS         (1u << 0)
#define USBCMD_HCRST      (1u << 1)
#define USBCMD_INTE       (1u << 2)
#define USBSTS_HCH        (1u << 0)   /* HC halted */
#define USBSTS_CNR        (1u << 11)  /* controller not ready */

/* PORTSC bits */
#define PORTSC_CCS        (1u << 0)   /* current connect status */
#define PORTSC_PED        (1u << 1)   /* port enabled */
#define PORTSC_PR         (1u << 4)   /* port reset */
#define PORTSC_SPEED(v)   (((v) >> 10) & 0xF)

/* Interrupter 0 register set (Runtime + 0x20) */
#define IR0_BASE          0x20
#define IR0_IMAN          0x00
#define IR0_IMOD          0x04
#define IR0_ERSTSZ        0x08
#define IR0_ERSTBA        0x10  /* u64 */
#define IR0_ERDP          0x18  /* u64 */

#define CMD_RING_TRBS     256
#define EVT_RING_TRBS     256
#define CRCR_RCS          (1u << 0)   /* ring cycle state */

static volatile uint8_t *xhci_base;   /* mapped BAR0 */
static uint32_t cap_len, rt_off, db_off;
static uint32_t max_slots, max_ports;
static int      ctx_csz64;            /* HCCPARAMS1.CSZ: 1 => 64-byte contexts */

/* DMA control structures (paddr handed to the controller). */
static volatile uint64_t *dcbaa;      static uint64_t dcbaa_pa;
static volatile uint8_t  *cmd_ring;   static uint64_t cmd_ring_pa;
static volatile uint8_t  *evt_ring;   static uint64_t evt_ring_pa;
static volatile uint8_t  *erst;       static uint64_t erst_pa;

/* ---- raw register access ---- */
static inline uint32_t r32(uint32_t off) { return *(volatile uint32_t *)(xhci_base + off); }
static inline void     w32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(xhci_base + off) = v; }
static inline uint32_t op_r32(uint32_t off) { return r32(cap_len + off); }
static inline void     op_w32(uint32_t off, uint32_t v) { w32(cap_len + off, v); }
/* 64-bit registers are written low-then-high (canonical, and some HCs require it) */
static inline void op_w64(uint32_t off, uint64_t v) {
    op_w32(off, (uint32_t)v); op_w32(off + 4, (uint32_t)(v >> 32));
}
static inline void ir0_w32(uint32_t off, uint32_t v) { w32(rt_off + IR0_BASE + off, v); }
static inline void ir0_w64(uint32_t off, uint64_t v) {
    w32(rt_off + IR0_BASE + off, (uint32_t)v);
    w32(rt_off + IR0_BASE + off + 4, (uint32_t)(v >> 32));
}

/* Allocate one zeroed, non-cacheable DMA page; returns vaddr + paddr. */
static void *dma_page(uint64_t *pa_out) {
    vka_object_t fr;
    if (vka_alloc_frame(&vka, seL4_PageBits, &fr)) return NULL;
    void *va = vspace_map_pages(&vspace, &fr.cptr, NULL,
                                seL4_AllRights, 1, seL4_PageBits, 0 /* non-cacheable */);
    if (!va) { vka_free_object(&vka, &fr); return NULL; }
    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(fr.cptr);
    if (ga.error) { return NULL; }
    for (int i = 0; i < 4096; i++) ((volatile uint8_t *)va)[i] = 0;
    *pa_out = ga.paddr;
    return va;
}

/* Map the controller's BAR0 register space (non-cacheable device memory). */
static int map_bar(void) {
    uint32_t pages = (uint32_t)((pcie_xhci_bar_size + 0xFFF) / 0x1000);
    if (pages == 0) pages = 1;
    if (pages > 8) pages = 8;   /* xHCI register space is small; cap defensively */
    seL4_CPtr caps[8];
    for (uint32_t i = 0; i < pages; i++) {
        vka_object_t fr;
        if (sel4platsupport_alloc_frame_at(&vka, pcie_xhci_bar + (uint64_t)i * 0x1000,
                                            seL4_PageBits, &fr)) {
            AIOS_LOG_ERROR("xHCI BAR frame alloc failed");
            return -1;
        }
        caps[i] = fr.cptr;
    }
    xhci_base = (volatile uint8_t *)vspace_map_pages(&vspace, caps, NULL,
                    seL4_AllRights, pages, seL4_PageBits, 0 /* non-cacheable */);
    if (!xhci_base) { AIOS_LOG_ERROR("xHCI BAR map failed"); return -1; }
    return 0;
}

int xhci_init(void) {
    if (!pcie_xhci_present) { AIOS_LOG_WARN("no xHCI controller"); return -1; }
    if (map_bar()) return -1;

    /* ---- B1: capability registers ----
     * Read CAPLENGTH (low byte) and HCIVERSION (high half) as one 32-bit dword:
     * some controllers only honour 32-bit accesses to the cap registers, so a
     * standalone 16-bit read of HCIVERSION at 0x02 can return 0. */
    uint32_t caplen_ver = r32(XHCI_CAPLENGTH);
    cap_len = caplen_ver & 0xFF;
    uint16_t version = (uint16_t)((caplen_ver >> 16) & 0xFFFF);
    uint32_t hcs1 = r32(XHCI_HCSPARAMS1);
    uint32_t hcc1 = r32(XHCI_HCCPARAMS1);
    max_slots = hcs1 & 0xFF;
    max_ports = (hcs1 >> 24) & 0xFF;
    ctx_csz64 = (hcc1 >> 2) & 1;
    db_off = r32(XHCI_DBOFF) & ~0x3u;
    rt_off = r32(XHCI_RTSOFF) & ~0x1Fu;
    printf("[xhci] v%x.%02x caplen=%u slots=%u ports=%u ctx=%u rtoff=0x%x dboff=0x%x\n",
           version >> 8, version & 0xFF, cap_len, max_slots, max_ports,
           ctx_csz64 ? 64 : 32, rt_off, db_off);

    /* ---- wait for CNR=0 (controller ready) ---- */
    for (uint64_t dl = mono_deadline_ms(1000); (op_r32(XHCI_USBSTS) & USBSTS_CNR); ) {
        if (!mono_before(dl)) { AIOS_LOG_ERROR("xHCI not ready (CNR)"); return -1; }
    }

    /* ---- reset: halt then HCRST ---- */
    uint32_t cmd = op_r32(XHCI_USBCMD);
    op_w32(XHCI_USBCMD, cmd & ~USBCMD_RS);     /* ensure stopped */
    for (uint64_t dl = mono_deadline_ms(1000); !(op_r32(XHCI_USBSTS) & USBSTS_HCH); )
        if (!mono_before(dl)) { AIOS_LOG_ERROR("xHCI halt timeout"); return -1; }
    op_w32(XHCI_USBCMD, USBCMD_HCRST);
    for (uint64_t dl = mono_deadline_ms(1000);
         (op_r32(XHCI_USBCMD) & USBCMD_HCRST) || (op_r32(XHCI_USBSTS) & USBSTS_CNR); )
        if (!mono_before(dl)) { AIOS_LOG_ERROR("xHCI reset timeout"); return -1; }
    AIOS_LOG_INFO("xHCI reset complete");

    /* ---- B2: DMA structures ----
     * DCBAA + ERST share one page (DCBAA at +0, ERST at +2048, both 64-aligned).
     * Command ring and event ring each get their own page. */
    dcbaa = (volatile uint64_t *)dma_page(&dcbaa_pa);
    cmd_ring = (volatile uint8_t *)dma_page(&cmd_ring_pa);
    evt_ring = (volatile uint8_t *)dma_page(&evt_ring_pa);
    if (!dcbaa || !cmd_ring || !evt_ring) { AIOS_LOG_ERROR("xHCI DMA alloc failed"); return -1; }
    erst    = (volatile uint8_t *)dcbaa + 2048;
    erst_pa = dcbaa_pa + 2048;

    /* Scratchpad buffers: DCBAA[0] points at the scratchpad-buffer-array.
     * QEMU reports 0, so DCBAA[0] stays 0; handle a nonzero count minimally. */
    uint32_t hcs2 = r32(XHCI_HCSPARAMS2);
    uint32_t spb = ((hcs2 >> 27) & 0x1F) << 5 | ((hcs2 >> 21) & 0x1F);
    if (spb > 0) {
        uint64_t arr_pa;
        volatile uint64_t *arr = (volatile uint64_t *)dma_page(&arr_pa);
        if (arr) {
            uint32_t n = spb > 256 ? 256 : spb;
            for (uint32_t i = 0; i < n; i++) {
                uint64_t bpa;
                if (!dma_page(&bpa)) break;
                arr[i] = bpa;
            }
            dcbaa[0] = arr_pa;
        }
        AIOS_LOG_INFO_V("xHCI scratchpad buffers=", spb);
    }

    /* ---- B3: program + run ---- */
    op_w32(XHCI_CONFIG, max_slots);             /* MaxSlotsEn */
    op_w64(XHCI_DCBAAP, dcbaa_pa);
    op_w64(XHCI_CRCR, cmd_ring_pa | CRCR_RCS);  /* command ring + cycle state */

    /* Event ring: one segment. ERST[0] = {evt_ring_pa, EVT_RING_TRBS, 0}. */
    *(volatile uint64_t *)(erst + 0) = evt_ring_pa;
    *(volatile uint32_t *)(erst + 8) = EVT_RING_TRBS;
    *(volatile uint32_t *)(erst + 12) = 0;
    ir0_w32(IR0_ERSTSZ, 1);
    ir0_w64(IR0_ERDP, evt_ring_pa);
    ir0_w64(IR0_ERSTBA, erst_pa);
    arch_dsb();

    /* Run. */
    op_w32(XHCI_USBCMD, USBCMD_RS);
    for (uint64_t dl = mono_deadline_ms(1000); (op_r32(XHCI_USBSTS) & USBSTS_HCH); )
        if (!mono_before(dl)) { AIOS_LOG_ERROR("xHCI run timeout (still halted)"); return -1; }

    /* ---- detect connected ports ---- */
    int connected = 0;
    for (uint32_t p = 0; p < max_ports; p++) {
        uint32_t sc = op_r32(XHCI_PORTSC(p));
        if (sc & PORTSC_CCS) {
            connected++;
            printf("[xhci] port %u: device connected (speed %u, portsc=0x%x)\n",
                   p + 1, PORTSC_SPEED(sc), sc);
        }
    }
    printf("[xhci] operational: %u ports, %d connected\n", max_ports, connected);
    return 0;
}

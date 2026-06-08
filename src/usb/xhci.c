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
static uint32_t xhci_bar_bytes;       /* bytes actually mapped (bounds-check rt/db off) */
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
/* Doorbell N (N=0 command ring; N=slot for device endpoints). */
static inline void doorbell(uint32_t n, uint32_t target) {
    *(volatile uint32_t *)(xhci_base + db_off + n * 4) = target;
}

/* ---- TRB types (control[15:10]) ---- */
#define TRB_NORMAL        1
#define TRB_SETUP         2
#define TRB_DATA          3
#define TRB_STATUS        4
#define TRB_LINK          6
#define TRB_ENABLE_SLOT   9
#define TRB_ADDRESS_DEV   11
#define TRB_CONFIG_EP     12
#define TRB_EVAL_CONTEXT  13
#define TRB_NOOP_CMD      23
#define TRB_TRANSFER_EVT  32
#define TRB_CMD_COMP_EVT  33
#define TRB_PORT_STS_EVT  34
#define TRB_TYPE(ctrl)    (((ctrl) >> 10) & 0x3F)
#define TRB_SET_TYPE(t)   ((uint32_t)(t) << 10)
#define TRB_CYCLE         (1u << 0)
#define EVT_CC(trb2)      (((trb2) >> 24) & 0xFF)  /* completion code in status dword */
#define EVT_SLOT(trb3)    (((trb3) >> 24) & 0xFF)  /* slot id in control dword */
#define CC_SUCCESS        1

/* ---- command ring + event ring producer/consumer state ---- */
static uint32_t cmd_enq = 0, cmd_cycle = 1;   /* command ring (producer) */
static uint32_t evt_deq = 0, evt_cycle = 1;   /* event ring (consumer) */

/* Non-blocking: if the next event ring TRB has been produced, copy it into out[]
 * and advance ERDP. Returns 0 on an event, -1 if none pending. */
static int evt_poll_once(uint32_t out[4]) {
    volatile uint32_t *trb = (volatile uint32_t *)(evt_ring + evt_deq * 16);
    uint32_t ctrl = trb[3];
    if ((ctrl & TRB_CYCLE) != evt_cycle) return -1;   /* not yet produced */
    out[0] = trb[0]; out[1] = trb[1]; out[2] = trb[2]; out[3] = ctrl;
    if (++evt_deq == EVT_RING_TRBS) { evt_deq = 0; evt_cycle ^= 1; }
    ir0_w64(IR0_ERDP, (evt_ring_pa + evt_deq * 16) | (1u << 3) /* EHB */);
    return 0;
}

/* Time-bounded blocking poll for the next event. */
static int evt_poll(uint32_t out[4], int timeout_ms) {
    for (uint64_t dl = mono_deadline_ms(timeout_ms); mono_before(dl); )
        if (evt_poll_once(out) == 0) return 0;
    return -1;
}

/* Submit a command TRB, ring doorbell 0, and wait for its Command Completion
 * Event (skipping any interleaved port-status events). Returns the completion
 * code (CC_SUCCESS=1) and, if evt!=NULL, the completion event TRB. -1 on timeout.
 * The command ring holds a Link TRB at the last slot; enumeration issues only a
 * handful of commands so the ring does not wrap here. */
static int cmd_submit(uint64_t param, uint32_t status, uint32_t type, uint32_t slot,
                      uint32_t evt[4]) {
    volatile uint32_t *trb = (volatile uint32_t *)(cmd_ring + cmd_enq * 16);
    trb[0] = (uint32_t)param;
    trb[1] = (uint32_t)(param >> 32);
    trb[2] = status;
    trb[3] = TRB_SET_TYPE(type) | (slot << 24) | cmd_cycle;
    arch_dsb();
    if (++cmd_enq >= CMD_RING_TRBS - 1) cmd_enq = 0;  /* simple bound (no wrap in use) */
    doorbell(0, 0);
    arch_dsb();

    uint32_t e[4];
    for (uint64_t dl = mono_deadline_ms(1000); mono_before(dl); ) {
        if (evt_poll(e, 200) != 0) continue;
        if (TRB_TYPE(e[3]) == TRB_CMD_COMP_EVT) {
            if (evt) { evt[0] = e[0]; evt[1] = e[1]; evt[2] = e[2]; evt[3] = e[3]; }
            return (int)EVT_CC(e[2]);
        }
        /* else: port-status or stray event -- already consumed, keep waiting */
    }
    return -1;
}

/* ---- low DMA pool (one 2 MB frame, carved into 4 KB pages) ----
 * The controller DMAs to the PADDRs we hand it. On the RPi4 the brcmstb inbound DMA
 * window is the low 3 GB (PCI [0, 0xC0000000)); the outbound window owns [0xC0000000,..).
 * A vka_alloc_frame run LATE returns pages ABOVE 3 GB (observed 0xfb940000) that the
 * controller cannot reach -> every command times out (cc=-1, no event). The fix:
 * reserve ONE 2 MB frame and REQUIRE it below 3 GB, then carve the rings/contexts from
 * it. Reserved EARLY (xhci_dma_reserve from aios_root, before fs/display eat low RAM)
 * so a low frame is available; a 2 MB frame also lets us read its paddr directly
 * (GetAddress) and cleanly retry-for-low. QEMU DMA is unrestricted, so any address works. */
#define DMA_LIMIT      0xC0000000ULL            /* top of the RPi4 inbound DMA window */
#define DMA_POOL_PAGES 512                       /* 2 MB / 4 KB */
static uint8_t  *dma_pool_va;                   /* contiguous mapped base (2 MB) */
static uint64_t  dma_pool_pa;                   /* contiguous base paddr (2 MB aligned) */
static uint32_t  dma_pool_used;

/* Reserve the xHCI DMA pool. Idempotent. Call EARLY (low RAM still plentiful) so the
 * 2 MB frame lands below 3 GB; a later call is a no-op. Retries a few times for a low
 * frame, releasing any high rejects. Returns 0 on success. */
int xhci_dma_reserve(void) {
    if (dma_pool_va) return 0;
    vka_object_t rejects[8];
    int nrej = 0, have = 0;
    vka_object_t fr;
    for (int t = 0; t < 8 && !have; t++) {
        if (vka_alloc_frame(&vka, seL4_LargePageBits, &fr)) break;   /* 2 MB frame */
        seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(fr.cptr);
        if (!ga.error && ga.paddr + 0x200000ULL <= DMA_LIMIT) {
            dma_pool_pa = ga.paddr; have = 1; break;
        }
        if (nrej < 8) rejects[nrej++] = fr; else vka_free_object(&vka, &fr);  /* keep -> next differs */
    }
    for (int i = 0; i < nrej; i++) vka_free_object(&vka, &rejects[i]);        /* release high rejects */
    if (!have) {
        /* No low frame -- accept whatever is left (QEMU is fine; the log flags RPi4). */
        if (vka_alloc_frame(&vka, seL4_LargePageBits, &fr)) {
            AIOS_LOG_ERROR("xHCI DMA pool: 2MB frame alloc failed"); return -1;
        }
        seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(fr.cptr);
        dma_pool_pa = ga.error ? 0 : ga.paddr;
    }
    dma_pool_va = (uint8_t *)vspace_map_pages(&vspace, &fr.cptr, NULL, seL4_AllRights,
                                1, seL4_LargePageBits, 0 /* non-cacheable */);
    if (!dma_pool_va) { AIOS_LOG_ERROR("xHCI DMA pool map failed"); return -1; }
    dma_pool_used = 0;
    int low = (dma_pool_pa + 0x200000ULL <= DMA_LIMIT);
    printf("[xhci] DMA pool: 2MB @ phys 0x%lx%s\n", (unsigned long)dma_pool_pa,
           low ? " (DMA-reachable)" : " (>3GB -- RPi4 DMA WILL FAIL)");
    return 0;
}

/* Carve one zeroed page from the low DMA pool; returns vaddr + paddr. */
static void *dma_page(uint64_t *pa_out) {
    if (!dma_pool_va || dma_pool_used >= DMA_POOL_PAGES) {
        AIOS_LOG_ERROR("xHCI DMA pool exhausted"); return NULL;
    }
    uint32_t i = dma_pool_used++;
    void *va = dma_pool_va + (uint64_t)i * 0x1000;
    for (int k = 0; k < 4096; k++) ((volatile uint8_t *)va)[k] = 0;
    *pa_out = dma_pool_pa + (uint64_t)i * 0x1000;
    return va;
}

/* Map the controller BAR0 register space (non-cacheable device memory).
 * Capped at 64 pages (256 KB): the QEMU qemu-xhci BAR is 16 KB, the RPi4 VL805 BAR is
 * up to 64 KB -- both well under the cap, which guards against a bogus huge size. On
 * the RPi4 the alloc at CPU 0x6_00000000 is also the live test that the PCIe outbound
 * window is backed by a device untyped (the D.2b question -- see aios_root.c). */
#define XHCI_BAR_MAX_PAGES 64
static int map_bar(void) {
    uint32_t pages = (uint32_t)((pcie_xhci_bar_size + 0xFFF) / 0x1000);
    if (pages == 0) pages = 1;
    if (pages > XHCI_BAR_MAX_PAGES) pages = XHCI_BAR_MAX_PAGES;
    seL4_CPtr caps[XHCI_BAR_MAX_PAGES];
    for (uint32_t i = 0; i < pages; i++) {
        vka_object_t fr;
        if (sel4platsupport_alloc_frame_at(&vka, pcie_xhci_bar + (uint64_t)i * 0x1000,
                                            seL4_PageBits, &fr)) {
            printf("[xhci] BAR frame alloc FAILED at CPU 0x%lx (page %u) -- the PCIe "
                   "window is not a mappable device untyped (needs D.2b kernel change)\n",
                   (unsigned long)(pcie_xhci_bar + (uint64_t)i * 0x1000), i);
            return -1;
        }
        caps[i] = fr.cptr;
    }
    xhci_base = (volatile uint8_t *)vspace_map_pages(&vspace, caps, NULL,
                    seL4_AllRights, pages, seL4_PageBits, 0 /* non-cacheable */);
    if (!xhci_base) { AIOS_LOG_ERROR("xHCI BAR map failed"); return -1; }
    xhci_bar_bytes = pages * 0x1000u;
    printf("[xhci] BAR mapped: CPU 0x%lx size 0x%lx (%u pages) -> %p\n",
           (unsigned long)pcie_xhci_bar, (unsigned long)pcie_xhci_bar_size,
           pages, (void *)xhci_base);
    return 0;
}

/* ---- addressed device state (single device for now: the keyboard) ---- */
static uint32_t dev_slot;
static volatile uint8_t *dev_ctx;  static uint64_t dev_ctx_pa;
static volatile uint8_t *in_ctx;   static uint64_t in_ctx_pa;
static volatile uint8_t *ep0_ring; static uint64_t ep0_ring_pa;
static uint32_t ep0_enq, ep0_cycle;

#define CTX_SZ        (ctx_csz64 ? 64u : 32u)
#define PORTSC_PRC    (1u << 21)   /* port reset change (W1C) */
#define PORTSC_PP     (1u << 9)    /* port power */

/* Reset a USB2 root-hub port so it advances to Enabled (speed becomes valid). */
static int port_reset(uint32_t p) {
    uint32_t sc = op_r32(XHCI_PORTSC(p));
    /* Set PR, keep PP; writing 0 to the W1C change bits does NOT clear them. */
    op_w32(XHCI_PORTSC(p), (sc & PORTSC_PP) | PORTSC_PR);
    for (uint64_t dl = mono_deadline_ms(500); mono_before(dl); ) {
        sc = op_r32(XHCI_PORTSC(p));
        if (sc & PORTSC_PRC) break;             /* reset complete */
        if (!(sc & PORTSC_CCS)) return -1;      /* device vanished */
    }
    op_w32(XHCI_PORTSC(p), (sc & PORTSC_PP) | PORTSC_PRC);  /* clear PRC (W1C) */
    sc = op_r32(XHCI_PORTSC(p));
    if (!(sc & PORTSC_PED)) AIOS_LOG_WARN("port not enabled after reset");
    return 0;
}

/* Enqueue one TRB on the EP0 transfer ring (simple bound; control transfers are
 * short, so the ring does not wrap during enumeration). */
static void ep0_enqueue(uint64_t param, uint32_t status, uint32_t control) {
    volatile uint32_t *trb = (volatile uint32_t *)(ep0_ring + ep0_enq * 16);
    trb[0] = (uint32_t)param;
    trb[1] = (uint32_t)(param >> 32);
    trb[2] = status;
    trb[3] = control | ep0_cycle;
    if (++ep0_enq >= 255) ep0_enq = 0;
}

/* EP0 control transfer (Setup/[Data]/Status). Returns the completion code
 * (CC_SUCCESS=1) or -1 on timeout. data_pa is the DMA buffer for the data stage. */
static int control_transfer(uint8_t bmReqType, uint8_t bReq, uint16_t wVal,
                            uint16_t wIdx, uint16_t wLen, uint64_t data_pa, int dir_in) {
    uint64_t setup = (uint64_t)bmReqType | ((uint64_t)bReq << 8) |
                     ((uint64_t)wVal << 16) | ((uint64_t)wIdx << 32) |
                     ((uint64_t)wLen << 48);
    uint32_t trt = wLen ? (dir_in ? 3u : 2u) : 0u;   /* transfer type: 3=IN,2=OUT,0=none */
    ep0_enqueue(setup, 8, TRB_SET_TYPE(TRB_SETUP) | (1u << 6) /* IDT */ | (trt << 16));
    if (wLen)
        ep0_enqueue(data_pa, wLen, TRB_SET_TYPE(TRB_DATA) | (dir_in ? (1u << 16) : 0));
    int status_in = wLen ? !dir_in : 1;
    ep0_enqueue(0, 0, TRB_SET_TYPE(TRB_STATUS) | (status_in ? (1u << 16) : 0) | (1u << 5) /* IOC */);
    arch_dsb();
    doorbell(dev_slot, 1);   /* DCI 1 = EP0 */
    arch_dsb();
    uint32_t e[4];
    for (uint64_t dl = mono_deadline_ms(1000); mono_before(dl); ) {
        if (evt_poll(e, 200) != 0) continue;
        if (TRB_TYPE(e[3]) == TRB_TRANSFER_EVT) return (int)EVT_CC(e[2]);
    }
    return -1;
}

/* ---- HID keyboard (Layer 4/5) ---- */
extern vka_object_t serial_ep;   /* tty input endpoint (defined in aios_root.c) */

static volatile uint8_t *int_ring; static uint64_t int_ring_pa;
static uint32_t int_enq, int_cycle;
static volatile uint8_t *rpt;      static uint64_t rpt_pa;   /* 8-byte boot report buffer */
static uint32_t kbd_dci, kbd_mps;
static int      kbd_iface;
static uint8_t  prev_keys[6];
int xhci_kbd_ok = 0;               /* read by boot_services to spawn the driver thread */

/* HID usage id -> ASCII for the non-letter keys (letters handled arithmetically). */
static const char hid_map[128] = {
    [0x1e]='1',[0x1f]='2',[0x20]='3',[0x21]='4',[0x22]='5',[0x23]='6',[0x24]='7',
    [0x25]='8',[0x26]='9',[0x27]='0',[0x28]='\n',[0x29]=0x1b,[0x2a]='\b',[0x2b]='\t',
    [0x2c]=' ',[0x2d]='-',[0x2e]='=',[0x2f]='[',[0x30]=']',[0x31]='\\',[0x33]=';',
    [0x34]='\'',[0x35]='`',[0x36]=',',[0x37]='.',[0x38]='/',
};
static const char hid_map_shift[128] = {
    [0x1e]='!',[0x1f]='@',[0x20]='#',[0x21]='$',[0x22]='%',[0x23]='^',[0x24]='&',
    [0x25]='*',[0x26]='(',[0x27]=')',[0x28]='\n',[0x29]=0x1b,[0x2a]='\b',[0x2b]='\t',
    [0x2c]=' ',[0x2d]='_',[0x2e]='+',[0x2f]='{',[0x30]='}',[0x31]='|',[0x33]=':',
    [0x34]='"',[0x35]='~',[0x36]='<',[0x37]='>',[0x38]='?',
};
static char hid_to_ascii(uint8_t kc, int shift) {
    if (kc >= 0x04 && kc <= 0x1d) { char c = 'a' + (kc - 0x04); return shift ? c - 32 : c; }
    if (kc >= 128) return 0;
    return shift ? hid_map_shift[kc] : hid_map[kc];
}

/* Re-arm one interrupt-IN transfer for the keyboard's report. */
static void arm_int(void) {
    volatile uint32_t *trb = (volatile uint32_t *)(int_ring + int_enq * 16);
    trb[0] = (uint32_t)rpt_pa;
    trb[1] = (uint32_t)(rpt_pa >> 32);
    trb[2] = 8;                                   /* boot report = 8 bytes */
    trb[3] = TRB_SET_TYPE(TRB_NORMAL) | (1u << 5) /* IOC */ | int_cycle;
    /* Wrap via a Link TRB at the last slot so the ring cycles indefinitely
     * (a keyboard runs forever -- without this it breaks after ~255 reports). */
    if (++int_enq == 255) {
        volatile uint32_t *link = (volatile uint32_t *)(int_ring + 255 * 16);
        link[0] = (uint32_t)int_ring_pa;
        link[1] = (uint32_t)(int_ring_pa >> 32);
        link[2] = 0;
        link[3] = TRB_SET_TYPE(TRB_LINK) | (1u << 1) /* Toggle Cycle */ | int_cycle;
        int_enq = 0;
        int_cycle ^= 1;
    }
    arch_dsb();
    doorbell(dev_slot, kbd_dci);
    arch_dsb();
}

/* Decode an 8-byte boot report and feed newly-pressed keys to the tty. */
static void process_report(void) {
    int shift = (rpt[0] & 0x22) != 0;             /* L/R Shift modifier bits */
    for (int i = 2; i < 8; i++) {
        uint8_t kc = rpt[i];
        if (!kc) continue;
        int was = 0;
        for (int j = 0; j < 6; j++) if (prev_keys[j] == kc) was = 1;
        if (was) continue;                        /* still held -- not a new press */
        char ch = hid_to_ascii(kc, shift);
        if (!ch) continue;
        printf("[xhci-kbd] key=0x%02x '%c'\n", kc, (ch >= 32 && ch < 127) ? ch : '?');
        seL4_SetMR(0, (seL4_Word)(unsigned char)ch);
        seL4_Call(serial_ep.cptr, seL4_MessageInfo_new(SER_KEY_PUSH, 0, 0, 1));
    }
    for (int i = 0; i < 6; i++) prev_keys[i] = rpt[i + 2];
}

/* Read the config descriptor, find the HID boot-keyboard interrupt-IN endpoint,
 * SET_CONFIGURATION, Configure Endpoint, SET_PROTOCOL/SET_IDLE, and arm the first
 * interrupt transfer. Returns 0 with xhci_kbd_ok set on success. */
static int setup_keyboard(void) {
    uint64_t buf_pa;
    volatile uint8_t *buf = (volatile uint8_t *)dma_page(&buf_pa);
    if (!buf) return -1;
    int cc = control_transfer(0x80, 6, (2u << 8), 0, 9, buf_pa, 1);    /* config hdr */
    if (cc != CC_SUCCESS) return -1;
    uint16_t total = buf[2] | (buf[3] << 8);
    uint8_t cfg_val = buf[5];
    if (total > 4096) total = 4096;
    cc = control_transfer(0x80, 6, (2u << 8), 0, total, buf_pa, 1);    /* full config */
    if (cc != CC_SUCCESS) return -1;

    int iface = -1, ep_addr = -1, ep_mps = 8, ep_interval = 0, in_hid = 0;
    for (int i = 0; i + 2 <= total; ) {
        uint8_t len = buf[i], type = buf[i + 1];
        if (len == 0) break;
        if (type == 4) {                          /* interface descriptor */
            in_hid = (buf[i + 5] == 3 && buf[i + 6] == 1 && buf[i + 7] == 1);
            if (in_hid) iface = buf[i + 2];
        } else if (type == 5 && in_hid) {         /* endpoint descriptor */
            uint8_t addr = buf[i + 2], attr = buf[i + 3];
            if ((addr & 0x80) && (attr & 0x3) == 3) {   /* interrupt IN */
                ep_addr = addr;
                ep_mps = buf[i + 4] | (buf[i + 5] << 8);
                ep_interval = buf[i + 6];
                break;
            }
        }
        i += len;
    }
    if (ep_addr < 0) { AIOS_LOG_WARN("no HID keyboard endpoint"); return -1; }
    kbd_iface = iface;
    kbd_mps = (uint32_t)ep_mps;
    kbd_dci = (uint32_t)((ep_addr & 0xF) * 2 + 1);   /* IN endpoint DCI */
    printf("[xhci] HID keyboard: iface=%d ep=0x%02x mps=%u interval=%d dci=%u\n",
           iface, ep_addr, kbd_mps, ep_interval, kbd_dci);

    cc = control_transfer(0x00, 9 /* SET_CONFIGURATION */, cfg_val, 0, 0, 0, 0);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("SET_CONFIG cc=", (unsigned long)cc); return -1; }

    int_ring = (volatile uint8_t *)dma_page(&int_ring_pa);
    rpt = (volatile uint8_t *)dma_page(&rpt_pa);
    if (!int_ring || !rpt) return -1;
    int_enq = 0; int_cycle = 1;

    /* Configure Endpoint: add slot ctx (A0) + the interrupt EP ctx (A[dci]). */
    volatile uint32_t *icc      = (volatile uint32_t *)(in_ctx + 0);
    volatile uint32_t *slot_ctx = (volatile uint32_t *)(in_ctx + CTX_SZ);
    volatile uint32_t *ep_ctx   = (volatile uint32_t *)(in_ctx + (1 + kbd_dci) * CTX_SZ);
    icc[0] = 0;
    icc[1] = (1u << 0) | (1u << kbd_dci);
    slot_ctx[0] = (slot_ctx[0] & ~(0x1Fu << 27)) | (kbd_dci << 27);  /* context entries */
    ep_ctx[0] = (uint32_t)(ep_interval & 0xFF) << 16;
    ep_ctx[1] = (7u << 3) | (3u << 1) | (kbd_mps << 16);   /* Interrupt IN, CErr=3, MPS */
    ep_ctx[2] = (uint32_t)(int_ring_pa | 1);               /* TR dequeue | DCS */
    ep_ctx[3] = (uint32_t)(int_ring_pa >> 32);
    ep_ctx[4] = 8 | (kbd_mps << 16);                       /* avg TRB len | max ESIT */
    arch_dsb();
    uint32_t evt[4];
    cc = cmd_submit(in_ctx_pa, 0, TRB_CONFIG_EP, dev_slot, evt);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("Configure EP cc=", (unsigned long)cc); return -1; }

    control_transfer(0x21, 0x0B /* SET_PROTOCOL */, 0 /* boot */, kbd_iface, 0, 0, 0);
    control_transfer(0x21, 0x0A /* SET_IDLE */, 0, kbd_iface, 0, 0, 0);

    arm_int();
    xhci_kbd_ok = 1;
    printf("[xhci] HID keyboard ready (polling)\n");
    return 0;
}

/* Keyboard driver thread: poll the event ring for interrupt-transfer completions,
 * decode each report, and re-arm. Yields when idle (IRQ delivery is a follow-up). */
void xhci_kbd_driver_fn(void *a, void *b, void *c) {
    (void)a; (void)b; (void)c;
    uint32_t e[4];
    while (1) {
        if (evt_poll_once(e) == 0) {
            if (TRB_TYPE(e[3]) == TRB_TRANSFER_EVT) {
                process_report();
                arm_int();
            }
        } else {
            seL4_Yield();
        }
    }
}

static void xhci_mdelay(int ms) { for (uint64_t dl = mono_deadline_ms(ms); mono_before(dl); ) {} }

/* Enable a slot, set up its slot + EP0 contexts, Address Device, and read the 18-byte
 * device descriptor into desc[]. Handles a device behind a hub: route = the path
 * through the hub (the downstream port number for a single tier), root_port = the root
 * hub port the topology hangs off, and parent_slot/parent_port give the xHC the TT
 * info it needs for a LS/FS device behind a HS hub. route=0, parent_slot=0 for a device
 * directly on a root port. Repurposes the single-device globals (dev_slot/ep0_ring/...).
 * Returns the device class (desc[4]) or -1. */
static int address_and_describe(uint32_t route, uint32_t root_port, uint32_t speed,
                                uint32_t parent_slot, uint32_t parent_port,
                                uint8_t desc[18]) {
    uint32_t evt[4];
    int cc = cmd_submit(0, 0, TRB_ENABLE_SLOT, 0, evt);
    if (cc != CC_SUCCESS) {
        /* cc=-1 = no Command Completion Event (doorbell/ring/DMA path); cc>0 = a
         * completion code. USBSTS HSE(bit2)/HCE(bit12) flags a host/DMA fault. */
        printf("[xhci] Enable Slot FAILED cc=%d USBSTS=0x%x (HSE=%d HCE=%d)\n",
               cc, op_r32(XHCI_USBSTS), (op_r32(XHCI_USBSTS) >> 2) & 1,
               (op_r32(XHCI_USBSTS) >> 12) & 1);
        return -1;
    }
    dev_slot = EVT_SLOT(evt[3]);

    dev_ctx  = (volatile uint8_t *)dma_page(&dev_ctx_pa);
    in_ctx   = (volatile uint8_t *)dma_page(&in_ctx_pa);
    ep0_ring = (volatile uint8_t *)dma_page(&ep0_ring_pa);
    if (!dev_ctx || !in_ctx || !ep0_ring) return -1;
    ep0_enq = 0; ep0_cycle = 1;
    dcbaa[dev_slot] = dev_ctx_pa;

    /* EP0 max packet: SS=512, HS=64 are fixed; LS=8; FS is unknown until we read the
     * descriptor, so start at 8 (the safe minimum) and correct it below. */
    uint32_t mps = (speed == 4) ? 512 : (speed == 3) ? 64 : 8;
    volatile uint32_t *icc      = (volatile uint32_t *)(in_ctx + 0);
    volatile uint32_t *slot_ctx = (volatile uint32_t *)(in_ctx + CTX_SZ);
    volatile uint32_t *ep0_ctx  = (volatile uint32_t *)(in_ctx + 2 * CTX_SZ);
    icc[1] = 0x3;                                  /* Add slot ctx + EP0 ctx */
    slot_ctx[0] = (1u << 27) | (speed << 20) | (route & 0xFFFFF);  /* entries=1, speed, route */
    slot_ctx[1] = (root_port & 0xFF) << 16;        /* root hub port number (1-based) */
    if (parent_slot && (speed == 1 || speed == 2))  /* LS/FS behind a HS hub -> TT info */
        slot_ctx[2] = (parent_slot & 0xFF) | ((parent_port & 0xFF) << 8);
    ep0_ctx[1] = (4u << 3) | (3u << 1) | (mps << 16); /* EP type=Control, CErr=3, MPS */
    ep0_ctx[2] = (uint32_t)(ep0_ring_pa | 1);      /* TR dequeue ptr lo | DCS */
    ep0_ctx[3] = (uint32_t)(ep0_ring_pa >> 32);
    ep0_ctx[4] = 8;                                /* average TRB length */
    arch_dsb();

    cc = cmd_submit(in_ctx_pa, 0, TRB_ADDRESS_DEV, dev_slot, evt);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("Address Device cc=", (unsigned long)cc); return -1; }

    uint64_t buf_pa;
    volatile uint8_t *buf = (volatile uint8_t *)dma_page(&buf_pa);
    if (!buf) return -1;
    /* Read the first 8 bytes to learn the real EP0 max packet (FS devices vary 8..64),
     * then correct the EP0 context via Evaluate Context before the full read. */
    cc = control_transfer(0x80, 6, (1u << 8), 0, 8, buf_pa, 1);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("GET_DESCRIPTOR(8) cc=", (unsigned long)cc); return -1; }
    uint32_t real_mps = buf[7];
    if (speed == 1 && real_mps && real_mps != mps) {  /* FS: fix EP0 MPS */
        icc[0] = 0; icc[1] = (1u << 1);               /* A1 = EP0 */
        ep0_ctx[1] = (4u << 3) | (3u << 1) | (real_mps << 16);
        arch_dsb();
        cmd_submit(in_ctx_pa, 0, TRB_EVAL_CONTEXT, dev_slot, evt);
        mps = real_mps;
    }
    cc = control_transfer(0x80, 6, (1u << 8), 0, 18, buf_pa, 1);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("GET_DESCRIPTOR(18) cc=", (unsigned long)cc); return -1; }
    for (int i = 0; i < 18; i++) desc[i] = buf[i];
    printf("[xhci] device: slot=%u speed=%u mps0=%u VID=%04x PID=%04x class=%u\n",
           dev_slot, speed, mps, desc[8] | (desc[9] << 8), desc[10] | (desc[11] << 8), desc[4]);
    return desc[4];
}

/* USB hub (the device on the root port is a hub, class 9 -- e.g. the VL805 internal
 * USB 2.0 hub on the RPi4, behind which every Pi USB-A port hangs). Configure it,
 * power + scan its downstream ports, reset the one with a device, and enumerate that
 * device THROUGH the hub (route string + parent-hub TT). The single-device globals are
 * repurposed for the downstream slot once the hub is set up. */
static int setup_hub(uint32_t hub_root_port, uint32_t hub_speed) {
    (void)hub_speed;
    uint32_t hub_slot = dev_slot;
    uint64_t buf_pa;
    volatile uint8_t *buf = (volatile uint8_t *)dma_page(&buf_pa);
    if (!buf) return -1;

    /* Configure the hub: read the config header, SET_CONFIGURATION. */
    int cc = control_transfer(0x80, 6, (2u << 8), 0, 9, buf_pa, 1);
    if (cc != CC_SUCCESS) return -1;
    uint8_t cfg_val = buf[5];
    cc = control_transfer(0x00, 9 /* SET_CONFIGURATION */, cfg_val, 0, 0, 0, 0);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN("hub SET_CONFIG failed"); return -1; }

    /* Hub descriptor (class type 0x29) -> number of downstream ports + power-on time. */
    cc = control_transfer(0xA0, 6, (0x29u << 8), 0, 8, buf_pa, 1);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN("hub descriptor failed"); return -1; }
    uint32_t nports = buf[2];
    uint32_t pwr_good = buf[5];                      /* in 2 ms units */
    printf("[xhci] hub: %u downstream ports\n", nports);

    /* Mark the slot a hub (Hub bit + Number of Ports) so the xHC allocates the TT for
     * LS/FS downstream devices. Evaluate Context with the slot context (A0). */
    volatile uint32_t *icc      = (volatile uint32_t *)(in_ctx + 0);
    volatile uint32_t *slot_ctx = (volatile uint32_t *)(in_ctx + CTX_SZ);
    icc[0] = 0; icc[1] = (1u << 0);                  /* A0 = slot */
    slot_ctx[0] |= (1u << 26);                       /* Hub = 1 */
    slot_ctx[1] = (slot_ctx[1] & 0x00FFFFFF) | (nports << 24); /* Number of Ports */
    arch_dsb();
    uint32_t evt[4];
    cmd_submit(in_ctx_pa, 0, TRB_EVAL_CONTEXT, hub_slot, evt);  /* best effort */

    /* Power every downstream port, then wait power-on-to-power-good. */
    for (uint32_t port = 1; port <= nports; port++)
        control_transfer(0x23, 3 /* SET_FEATURE */, 8 /* PORT_POWER */, port, 0, 0, 0);
    xhci_mdelay((int)pwr_good * 2 + 100);

    /* Poll the downstream ports for a connected device. Real USB connect-debounce
     * takes ~100s of ms after power-on (QEMU is instant), so scan for up to ~2.5s,
     * trying each connected port once (a port may appear over time, and there may be
     * more than one device). */
    uint32_t tried = 0;
    for (uint64_t scan = mono_deadline_ms(2500); mono_before(scan); ) {
        for (uint32_t port = 1; port <= nports; port++) {
            if (tried & (1u << port)) continue;
            if (control_transfer(0xA3 /* GET_STATUS (other) */, 0, 0, port, 4, buf_pa, 1) != CC_SUCCESS)
                continue;
            uint32_t pstat = buf[0] | (buf[1] << 8);
            if (!(pstat & 0x1)) continue;            /* PORT_CONNECTION: no device yet */
            tried |= (1u << port);
            printf("[xhci] hub port %u: device connected (status=0x%x)\n", port, pstat);

            control_transfer(0x23, 3, 4 /* PORT_RESET */, port, 0, 0, 0);
            for (uint64_t dl = mono_deadline_ms(800); mono_before(dl); ) {
                if (control_transfer(0xA3, 0, 0, port, 4, buf_pa, 1) != CC_SUCCESS) break;
                if ((buf[2] | (buf[3] << 8)) & 0x10) break;   /* C_PORT_RESET */
                xhci_mdelay(10);
            }
            pstat = buf[0] | (buf[1] << 8);
            control_transfer(0x23, 1 /* CLEAR_FEATURE */, 20 /* C_PORT_RESET */, port, 0, 0, 0);
            if (!(pstat & 0x2)) { AIOS_LOG_WARN("hub port not enabled after reset"); continue; }
            uint32_t kspeed = (pstat & (1u << 9)) ? 2 /* LS */
                            : (pstat & (1u << 10)) ? 3 /* HS */ : 1 /* FS */;

            uint8_t kdesc[18];
            int kcls = address_and_describe(port, hub_root_port, kspeed, hub_slot, port, kdesc);
            if (kcls < 0) { AIOS_LOG_WARN("downstream device enum failed"); continue; }
            if (setup_keyboard() == 0) return 0;     /* keyboard armed -- done */
        }
        xhci_mdelay(50);
    }
    /* Nothing usable: dump each port's final status (PP bit8 = powered, CCS bit0 =
     * connected) to tell "not plugged into this hub" from a power/timing problem. */
    for (uint32_t port = 1; port <= nports; port++) {
        if (control_transfer(0xA3, 0, 0, port, 4, buf_pa, 1) == CC_SUCCESS)
            printf("[xhci] hub port %u: final status=0x%x\n", port, buf[0] | (buf[1] << 8));
    }
    AIOS_LOG_WARN("no HID keyboard found behind hub");
    return -1;
}

/* Reset the root port, address the device on it, and either arm it (HID keyboard) or
 * recurse into it (USB hub -- the RPi4 path: the keyboard is behind the VL805 hub). */
static int setup_device(uint32_t p) {
    if (port_reset(p)) return -1;
    uint32_t speed = PORTSC_SPEED(op_r32(XHCI_PORTSC(p)));
    uint8_t desc[18];
    int cls = address_and_describe(0, p + 1, speed, 0, 0, desc);
    if (cls < 0) return -1;
    if (cls == 9) return setup_hub(p + 1, speed);   /* hub -> enumerate downstream */
    return setup_keyboard();
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

    /* Defensive: the runtime + doorbell register windows MUST lie inside the mapped
     * BAR. A conformant controller guarantees this, but bail gracefully (rather than
     * fault on an unmapped access) if a mis-sized BAR put them out of range -- this
     * is the first real test of the RPi4 outbound-window MMIO path. */
    uint32_t rt_end = rt_off + IR0_BASE + 0x20;
    uint32_t db_end = db_off + (max_slots + 1) * 4;
    if (rt_end > xhci_bar_bytes || db_end > xhci_bar_bytes) {
        printf("[xhci] register offsets exceed mapped BAR (rt_end=0x%x db_end=0x%x "
               "mapped=0x%x) -- aborting\n", rt_end, db_end, xhci_bar_bytes);
        return -1;
    }

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
     * Command ring and event ring each get their own page. All carved from the low
     * DMA pool so the controller (RPi4 3GB inbound window) can reach them. Normally
     * reserved early (aios_root); this is the idempotent fallback. */
    if (xhci_dma_reserve()) return -1;
    dcbaa = (volatile uint64_t *)dma_page(&dcbaa_pa);
    cmd_ring = (volatile uint8_t *)dma_page(&cmd_ring_pa);
    evt_ring = (volatile uint8_t *)dma_page(&evt_ring_pa);
    if (!dcbaa || !cmd_ring || !evt_ring) { AIOS_LOG_ERROR("xHCI DMA alloc failed"); return -1; }
    erst    = (volatile uint8_t *)dcbaa + 2048;
    erst_pa = dcbaa_pa + 2048;
    /* The controller DMAs to these PADDRs via the inbound window (RC_BAR2, identity).
     * On the RPi4 they must lie inside that window (<4GB RAM) -- log them so a DMA
     * failure (Enable Slot timeout) can be told apart from an out-of-window ring. */
    printf("[xhci] rings (paddr): dcbaa=0x%lx cmd=0x%lx evt=0x%lx\n",
           (unsigned long)dcbaa_pa, (unsigned long)cmd_ring_pa, (unsigned long)evt_ring_pa);

    /* Scratchpad buffers: DCBAA[0] points at the scratchpad-buffer-array.
     * HCSPARAMS2 Max Scratchpad Bufs = (Hi[25:21] << 5) | Lo[31:27] (xHCI 5.3.4 /
     * Linux HCS_MAX_SCRATCHPAD). QEMU reports 0; the VL805 reports 31. (An earlier
     * Hi/Lo swap here computed 992 -> a malformed array = undefined HC behaviour.) */
    uint32_t hcs2 = r32(XHCI_HCSPARAMS2);
    uint32_t spb = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
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
    printf("[xhci] running: USBSTS=0x%x USBCMD=0x%x\n",
           op_r32(XHCI_USBSTS), op_r32(XHCI_USBCMD));

    /* ---- detect connected ports ---- */
    int connected = 0;
    int first_port = -1;   /* 0-based index of the first connected port */
    for (uint32_t p = 0; p < max_ports; p++) {
        uint32_t sc = op_r32(XHCI_PORTSC(p));
        if (sc & PORTSC_CCS) {
            connected++;
            if (first_port < 0) first_port = (int)p;
            printf("[xhci] port %u: device connected (speed %u, portsc=0x%x)\n",
                   p + 1, PORTSC_SPEED(sc), sc);
        }
    }
    printf("[xhci] operational: %u ports, %d connected\n", max_ports, connected);
    if (connected == 0 || first_port < 0) return 0;

    /* ---- C2/C3: enable slot, reset + address the device, read its descriptor ---- */
    setup_device((uint32_t)first_port);
    return 0;
}

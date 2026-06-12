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
#define IMAN_IP           (1u << 0)   /* interrupt pending (W1C) */
#define IMAN_IE           (1u << 1)   /* interrupt enable */

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
static inline uint32_t ir0_r32(uint32_t off) { return r32(rt_off + IR0_BASE + off); }
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
#define TRB_RESET_EP      14
#define TRB_SET_TR_DEQ    16
#define TRB_NOOP_CMD      23
#define TRB_TRANSFER_EVT  32
#define TRB_CMD_COMP_EVT  33
#define TRB_PORT_STS_EVT  34
#define TRB_TYPE(ctrl)    (((ctrl) >> 10) & 0x3F)
#define TRB_SET_TYPE(t)   ((uint32_t)(t) << 10)
#define TRB_CYCLE         (1u << 0)
#define EVT_CC(trb2)      (((trb2) >> 24) & 0xFF)  /* completion code in status dword */
#define EVT_SLOT(trb3)    (((trb3) >> 24) & 0xFF)  /* slot id in control dword [31:24] */
#define EVT_EPID(trb3)    (((trb3) >> 16) & 0x1F)  /* endpoint id (DCI) in transfer event [20:16] */
#define CC_SUCCESS        1
#define CC_STALL          6                        /* Stall Error -- endpoint halted */
#define CC_SHORT_PKT      13                       /* Short Packet -- benign */
/* v0.4.204: interrupt-IN error accounting. An error completion means the armed
 * buffer holds NO report -- decoding it replays stale keys (garbage input), and
 * blind re-arming can storm. Count, skip decode, and PARK the endpoint after a
 * storm threshold (the kbd is dead at that point; a replug/port-reset re-enables). */
#define INT_ERR_PARK_THRESHOLD  10000
static uint32_t g_int_err_events;

/* Forward decl: if (slot, ep) is the live keyboard's interrupt-IN endpoint, deliver
 * its report + re-arm and return 1, else 0. Defined in the HID section so the event
 * dispatcher below can keep the keyboard alive (even while a control transfer waits
 * on EP0) without pulling the HID globals up here. */
static int kbd_try_deliver(uint32_t slot, uint32_t ep, uint32_t cc);

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

/* Endpoint-aware event consumption. Consume ONE event off the ring (if any) and
 * dispatch it. The event ring is shared by EP0 control transfers, the command ring,
 * and the keyboard's interrupt-IN endpoint, but it has a SINGLE consumer at a time
 * (the init thread during enumeration; the driver thread at runtime -- they never
 * overlap). Routing by slot+endpoint means a control transfer waiting on EP0 neither
 * swallows nor is starved by the keyboard's interrupt-IN stream: a stray report is
 * delivered + re-armed here instead of being mistaken for the awaited completion.
 * Returns:
 *   DISP_MATCH  the awaited completion arrived (copied to out[])
 *   DISP_OTHER  some other event was consumed + handled (caller keeps waiting)
 *   DISP_NONE   no event pending
 * want_type 0 means "no awaited completion" -- the driver loop just pumps reports. */
#define DISP_NONE   (-1)
#define DISP_OTHER  0
#define DISP_MATCH  1
static void typematic_disarm_all(const char *why);

static int evt_dispatch(uint32_t out[4], uint32_t want_type,
                        uint32_t want_slot, uint32_t want_ep) {
    uint32_t e[4];
    if (evt_poll_once(e) != 0) return DISP_NONE;
    uint32_t type = TRB_TYPE(e[3]);
    if (type == TRB_TRANSFER_EVT) {
        uint32_t slot = EVT_SLOT(e[3]), ep = EVT_EPID(e[3]);
        if (want_type == TRB_TRANSFER_EVT && slot == want_slot && ep == want_ep) {
            out[0] = e[0]; out[1] = e[1]; out[2] = e[2]; out[3] = e[3];
            return DISP_MATCH;
        }
        kbd_try_deliver(slot, ep, EVT_CC(e[2]));   /* live report -- deliver + re-arm */
        return DISP_OTHER;
    }
    if (type == want_type) {         /* CMD_COMP_EVT for cmd_submit */
        out[0] = e[0]; out[1] = e[1]; out[2] = e[2]; out[3] = e[3];
        return DISP_MATCH;
    }
    /* Port status change = a device reset/connected/disconnected. Any armed
     * typematic may belong to a keyboard that just died mid-press (its release
     * will never arrive) -- disarm rather than risk a repeat runaway (v0.4.197). */
    if (type == TRB_PORT_STS_EVT)
        typematic_disarm_all("port change");
    return DISP_OTHER;               /* port-status / stray -- consumed, ignored */
}

/* Submit a command TRB, ring doorbell 0, and wait for its Command Completion Event
 * (the dispatcher delivers any interleaved keyboard report meanwhile). slot/ep go in
 * the TRB control dword [31:24]/[20:16] -- ep is 0 for slot-only commands, the EP DCI
 * for Reset Endpoint / Set TR Dequeue Pointer. Returns the completion code
 * (CC_SUCCESS=1) and, if evt!=NULL, the completion event TRB. -1 on timeout. The
 * command ring holds a Link TRB at the last slot; enumeration issues only a handful
 * of commands so the ring does not wrap here. */
static int cmd_submit(uint64_t param, uint32_t status, uint32_t type, uint32_t slot,
                      uint32_t ep, uint32_t evt[4]) {
    volatile uint32_t *trb = (volatile uint32_t *)(cmd_ring + cmd_enq * 16);
    trb[0] = (uint32_t)param;
    trb[1] = (uint32_t)(param >> 32);
    trb[2] = status;
    trb[3] = TRB_SET_TYPE(type) | ((ep & 0x1F) << 16) | (slot << 24) | cmd_cycle;
    arch_dsb();
    if (++cmd_enq >= CMD_RING_TRBS - 1) cmd_enq = 0;  /* simple bound (no wrap in use) */
    doorbell(0, 0);
    arch_dsb();

    uint32_t e[4];
    for (uint64_t dl = mono_deadline_ms(1000); mono_before(dl); ) {
        if (evt_dispatch(e, TRB_CMD_COMP_EVT, 0, 0) == DISP_MATCH) {
            if (evt) { evt[0] = e[0]; evt[1] = e[1]; evt[2] = e[2]; evt[3] = e[3]; }
            return (int)EVT_CC(e[2]);
        }
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

/* ---- per-device state ----
 * One entry per addressed USB device (keyboard, mouse, hub, ...). The DCBAA already
 * indexes device contexts by slot, so the controller is inherently multi-device; this
 * array makes the DRIVER multi-device. control_transfer / arm_int / process_report /
 * set_leds take a usb_dev*, so a RUNTIME control transfer (e.g. a keyboard SET_REPORT)
 * uses THAT device's EP0 ring even after other devices have enumerated. The event ring
 * still has a single consumer thread (enum on init, runtime on the driver thread), so
 * the array needs no locking. */
enum { USB_NONE = 0, USB_KBD = 1, USB_MOUSE = 2, USB_HUB = 9 };
#define MAX_USB_DEV  8
/* v0.4.192: keep MULTIPLE interrupt-IN transfers armed at once. The keyboard is a
 * low-speed device behind the VL805's transaction-translator (10ms interval) and
 * resets itself (LED out, input dead) if the host stops polling it even briefly --
 * which happened on ~the first key because the driver re-armed only AFTER the
 * blocking SER_KEY_PUSH echo, leaving a window with ZERO armed transfers. Arming a
 * small ring of report buffers means there are always (N-1) transfers pending while
 * the driver is busy, so the controller never stops polling the keyboard -> it never
 * starves. Buffers are carved from the existing (non-cacheable) rpt DMA page. */
#define INT_RING_BUFS  32    /* v0.4.197: was 8 -- fast typing + slow login-time
                              * echoes can drain 8 while the driver blocks in one
                              * echo Call; an EMPTY ring stops the controller
                              * polling the LS-kbd-behind-the-TT -> device reset
                              * (HW-seen: died mid-"root" at the login prompt).
                              * The rpt page holds 64 x RPT_STRIDE buffers; 32
                              * puts the cliff far beyond any human burst. */
#define RPT_STRIDE     64    /* per-report buffer stride within the rpt page */

/* Typematic (host-side key repeat) timing -- classic PC defaults. */
#define KBD_REPEAT_DELAY_MS  500   /* hold this long before repeating  */
#define KBD_REPEAT_RATE_MS   66    /* then ~15 chars per second        */
/* Dead-man cap (v0.4.197): if the keyboard dies BETWEEN a press and its release
 * (the recurring TT-death mode), the release never arrives and repeat would run
 * FOREVER -- the "rrrr" storm whose echo/scroll/auth load saturates core 0 and
 * takes the net down with it (HW-seen on builds 2045/2046). A held key on a
 * healthy keyboard also sends no reports (boot protocol is change-only), so the
 * guard is a generous cap on CONSECUTIVE repeats with no intervening report
 * from that device: ~20s at 15cps. Real >20s holds are rare; a dead keyboard
 * stops spamming. Any genuine report (incl. the eventual release) resets it. */
#define KBD_REPEAT_MAX_RUN   300

struct usb_dev {
    int      in_use;
    int      kind;                                  /* USB_KBD / USB_MOUSE / USB_HUB */
    uint32_t slot;
    uint32_t speed;                                 /* PORTSC speed: 1 FS, 2 LS, 3 HS, 4 SS */
    /* EP0 control endpoint */
    volatile uint8_t *dev_ctx;  uint64_t dev_ctx_pa;   /* output device context */
    volatile uint8_t *in_ctx;   uint64_t in_ctx_pa;    /* input context (Address/Configure) */
    volatile uint8_t *ep0_ring; uint64_t ep0_ring_pa;
    uint32_t ep0_enq, ep0_cycle;
    uint32_t mps0;                                  /* EP0 max packet */
    uint32_t iface;                                 /* HID interface number */
    /* interrupt-IN endpoint (HID boot report) */
    volatile uint8_t *int_ring; uint64_t int_ring_pa;
    uint32_t int_enq, int_cycle;
    uint32_t int_proc;                              /* FIFO: next report buffer to consume */
    volatile uint8_t *rpt;      uint64_t rpt_pa;    /* page holding INT_RING_BUFS buffers */
    uint32_t dci;                                   /* interrupt-IN endpoint DCI */
    uint32_t int_mps;
    int      rpt_len;                               /* boot report bytes: 8 kbd, 4 mouse */
    /* keyboard state */
    uint8_t  prev_keys[6];
    int      num_lock, caps_lock, scroll_lock;
    volatile uint8_t *led_buf;  uint64_t led_buf_pa;
    /* v0.4.192 typematic (host-side key repeat): the HID boot keyboard reports only
     * state CHANGES, so holding a key emits one report -- repeat must be host-driven.
     * The latest printable press arms repeat; its release disarms. The driver loop
     * emits the char again after KBD_REPEAT_DELAY_MS, then every KBD_REPEAT_RATE_MS. */
    int      rep_active;
    uint8_t  rep_kc;            /* usage code armed for repeat */
    char     rep_ch;            /* decoded char to re-emit (modifiers at press) */
    uint64_t rep_deadline;      /* mono_ticks deadline for the next emit */
    uint32_t rep_run;           /* consecutive emits since the last real report
                                 * (dead-man: see KBD_REPEAT_MAX_RUN) */
    /* mouse state */
    uint8_t  prev_btn;
};
static struct usb_dev g_devs[MAX_USB_DEV];

/* Allocate a zeroed device slot (Num Lock defaults on). NULL if the table is full. */
static struct usb_dev *dev_alloc(void) {
    for (int i = 0; i < MAX_USB_DEV; i++) {
        if (g_devs[i].in_use) continue;
        struct usb_dev *d = &g_devs[i];
        *d = (struct usb_dev){0};
        d->in_use = 1;
        d->num_lock = 1;
        return d;
    }
    AIOS_LOG_WARN("USB device table full");
    return NULL;
}

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

/* Enqueue one TRB on the EP0 transfer ring. The ring wraps via a Link TRB at the last
 * slot (with Toggle Cycle), so it cycles indefinitely: enumeration uses only a handful
 * of TRBs, but a long-lived device that issues runtime control transfers (e.g. a
 * keyboard's SET_REPORT for the lock LEDs, 3 TRBs each) would otherwise wedge EP0 once
 * ep0_enq passed 255 -- the controller would stall on a stale, never-toggled cycle. */
static void ep0_enqueue(struct usb_dev *d, uint64_t param, uint32_t status, uint32_t control) {
    volatile uint32_t *trb = (volatile uint32_t *)(d->ep0_ring + d->ep0_enq * 16);
    trb[0] = (uint32_t)param;
    trb[1] = (uint32_t)(param >> 32);
    trb[2] = status;
    trb[3] = control | d->ep0_cycle;
    if (++d->ep0_enq == 255) {
        volatile uint32_t *link = (volatile uint32_t *)(d->ep0_ring + 255 * 16);
        link[0] = (uint32_t)d->ep0_ring_pa;
        link[1] = (uint32_t)(d->ep0_ring_pa >> 32);
        link[2] = 0;
        link[3] = TRB_SET_TYPE(TRB_LINK) | (1u << 1) /* Toggle Cycle */ | d->ep0_cycle;
        d->ep0_enq = 0;
        d->ep0_cycle ^= 1;
    }
}

/* EP0 control transfer (Setup/[Data]/Status) on device d. Returns the completion code
 * (CC_SUCCESS=1) or -1 on timeout. data_pa is the DMA buffer for the data stage. */
static int control_transfer(struct usb_dev *d, uint8_t bmReqType, uint8_t bReq, uint16_t wVal,
                            uint16_t wIdx, uint16_t wLen, uint64_t data_pa, int dir_in) {
    uint64_t setup = (uint64_t)bmReqType | ((uint64_t)bReq << 8) |
                     ((uint64_t)wVal << 16) | ((uint64_t)wIdx << 32) |
                     ((uint64_t)wLen << 48);
    uint32_t trt = wLen ? (dir_in ? 3u : 2u) : 0u;   /* transfer type: 3=IN,2=OUT,0=none */
    ep0_enqueue(d, setup, 8, TRB_SET_TYPE(TRB_SETUP) | (1u << 6) /* IDT */ | (trt << 16));
    if (wLen)
        ep0_enqueue(d, data_pa, wLen, TRB_SET_TYPE(TRB_DATA) | (dir_in ? (1u << 16) : 0));
    int status_in = wLen ? !dir_in : 1;
    ep0_enqueue(d, 0, 0, TRB_SET_TYPE(TRB_STATUS) | (status_in ? (1u << 16) : 0) | (1u << 5) /* IOC */);
    arch_dsb();
    doorbell(d->slot, 1);   /* DCI 1 = EP0 */
    arch_dsb();
    uint32_t e[4];
    for (uint64_t dl = mono_deadline_ms(1000); mono_before(dl); ) {
        /* Wait for THIS device's EP0 completion; the dispatcher delivers any other
         * device's interrupt-IN report that arrives meanwhile rather than confusing it
         * for ours. */
        if (evt_dispatch(e, TRB_TRANSFER_EVT, d->slot, 1) == DISP_MATCH)
            return (int)EVT_CC(e[2]);
    }
    return -1;
}

/* ---- HID (Layer 4/5): keyboard + mouse ---- */
extern vka_object_t serial_ep;   /* tty input endpoint (defined in aios_root.c) */

int xhci_kbd_ok = 0;               /* read by boot_services to spawn the driver thread */

/* Lock-LED request channel for /proc/xhci (see set_leds). The diagnostic runs on the
 * fs/proc thread; it must not touch the event ring, so it only pokes g_led_request and
 * the driver thread issues the SET_REPORT in its OWN (single-consumer) context. Bit 8 =
 * pending; bits [2:0] = the Num/Caps/Scroll bitmap (bit 9 = "use current lock state"). */
static volatile uint32_t g_led_request;     /* 0x100=pending, 0x200=use-current, [2:0]=bits */
/* v0.4.203: per-event HID logging is OFF by default -- a 40-char printf to the
 * POLLED mini UART costs ~3.5ms of driver-thread busy-wait PER KEYSTROKE (plus
 * an HDMI line render + scroll when the screen is full), which drains the
 * interrupt ring under load and drops keys. /proc/xhci.debug.1 re-enables. */
static volatile int g_hid_debug;
static uint32_t g_key_events, g_mouse_events_logged;
static volatile uint32_t g_led_last_cc;     /* last SET_REPORT completion code (diag) */
static volatile uint32_t g_led_last_sts;    /* USBSTS right after last SET_REPORT (diag) */

/* "System mouse" snapshot updated by any mouse's reports (Task 4 consumer), exposed at
 * /proc/mouse so the shell / programs can read the cursor + buttons. x/y accumulate the
 * relative deltas, clamped to a virtual 1920x1080 extent. A framebuffer cursor drawn via
 * display_server is a noted follow-up that would read this same state. */
#define MOUSE_X_MAX 1919
#define MOUSE_Y_MAX 1079
static volatile int      g_mouse_x, g_mouse_y;   /* accumulated cursor, clamped */
static volatile uint32_t g_mouse_btn;            /* current button bitmap (bit0 L, 1 R, 2 M) */
static volatile uint32_t g_mouse_events;         /* reports with motion or a button change */

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
static char hid_to_ascii(struct usb_dev *d, uint8_t kc, int shift) {
    /* Letters a-z: Caps Lock inverts the shift. */
    if (kc >= 0x04 && kc <= 0x1d) {
        char c = 'a' + (kc - 0x04);
        return (shift ^ d->caps_lock) ? c - 32 : c;
    }
    /* Keypad (0x54-0x63). Operators + Enter are independent of Num Lock; the digit
     * and '.' keys type only when Num Lock is on (otherwise they are navigation keys
     * we do not map yet). */
    switch (kc) {
        case 0x54: return '/';
        case 0x55: return '*';
        case 0x56: return '-';
        case 0x57: return '+';
        case 0x58: return '\n';                       /* Keypad Enter */
        case 0x59: return d->num_lock ? '1' : 0;
        case 0x5a: return d->num_lock ? '2' : 0;
        case 0x5b: return d->num_lock ? '3' : 0;
        case 0x5c: return d->num_lock ? '4' : 0;
        case 0x5d: return d->num_lock ? '5' : 0;
        case 0x5e: return d->num_lock ? '6' : 0;
        case 0x5f: return d->num_lock ? '7' : 0;
        case 0x60: return d->num_lock ? '8' : 0;
        case 0x61: return d->num_lock ? '9' : 0;
        case 0x62: return d->num_lock ? '0' : 0;
        case 0x63: return d->num_lock ? '.' : 0;
    }
    if (kc >= 128) return 0;
    return shift ? hid_map_shift[kc] : hid_map[kc];
}

/* Recover EP0 from a Halted state after a STALL: Reset Endpoint moves it Halted->Stopped,
 * then Set TR Dequeue Pointer resumes it at the current EP0 enqueue cursor (control_transfer
 * already advanced past the failed Setup/Data/Status) with the live cycle bit. Without this
 * a single STALLed SET_REPORT would wedge EP0 for all subsequent control transfers. */
static void ep0_recover(struct usb_dev *d) {
    uint32_t evt[4];
    cmd_submit(0, 0, TRB_RESET_EP, d->slot, 1 /* EP0 DCI */, evt);
    uint64_t deq = d->ep0_ring_pa + (uint64_t)d->ep0_enq * 16;
    cmd_submit(deq | d->ep0_cycle, 0, TRB_SET_TR_DEQ, d->slot, 1, evt);
}

/* Drive the keyboard lock LEDs: send a 1-byte HID Output report (bit0 Num, 1 Caps,
 * 2 Scroll) via SET_REPORT on EP0. MUST run on the event-ring's single consumer
 * thread (the driver thread at runtime, or the init thread at enumeration) -- never
 * the proc thread; see g_led_request. control_transfer is endpoint-aware, so a
 * keyboard report arriving mid-transfer is delivered, not lost. Records the
 * completion code + USBSTS (HSE/HCE) so a HW regression -- the reason the LED was
 * deferred in v0.4.185 -- is diagnosable via /proc/xhci, and recovers EP0 on a STALL. */
static void set_leds(struct usb_dev *d, int bits) {
    if (!d->led_buf) return;
    d->led_buf[0] = (uint8_t)(bits & 0x7);
    arch_dsb();
    int cc = control_transfer(d, 0x21, 0x09 /* SET_REPORT */, 0x0200 /* Output report, id 0 */,
                              d->iface, 1, d->led_buf_pa, 0 /* OUT */);
    uint32_t sts = op_r32(XHCI_USBSTS);
    g_led_last_cc = (uint32_t)cc;
    g_led_last_sts = sts;
    if (cc != CC_SUCCESS || (sts & ((1u << 2) | (1u << 12))))
        printf("[xhci-led] SET_REPORT leds=0x%02x cc=%d USBSTS=0x%x (HSE=%d HCE=%d)\n",
               d->led_buf[0], cc, sts, (sts >> 2) & 1, (sts >> 12) & 1);
    if (cc == CC_STALL) ep0_recover(d);
}

/* Apply the current software lock state to the LEDs (used on a lock-key press and at
 * enumeration). */
static void set_leds_current(struct usb_dev *d) {
    set_leds(d, (d->num_lock ? 1 : 0) | (d->caps_lock ? 2 : 0) | (d->scroll_lock ? 4 : 0));
}

/* Re-arm one interrupt-IN transfer for device d's boot report. */
static void arm_int_buf(struct usb_dev *d, uint32_t buf_idx) {
    uint64_t pa = d->rpt_pa + (uint64_t)buf_idx * RPT_STRIDE;
    volatile uint32_t *trb = (volatile uint32_t *)(d->int_ring + d->int_enq * 16);
    trb[0] = (uint32_t)pa;
    trb[1] = (uint32_t)(pa >> 32);
    trb[2] = (uint32_t)d->rpt_len;                /* boot report length (8 kbd, 4 mouse) */
    trb[3] = TRB_SET_TYPE(TRB_NORMAL) | (1u << 5) /* IOC */ | d->int_cycle;
    /* Wrap via a Link TRB at the last slot so the ring cycles indefinitely
     * (an input device runs forever -- without this it breaks after ~255 reports). */
    if (++d->int_enq == 255) {
        volatile uint32_t *link = (volatile uint32_t *)(d->int_ring + 255 * 16);
        link[0] = (uint32_t)d->int_ring_pa;
        link[1] = (uint32_t)(d->int_ring_pa >> 32);
        link[2] = 0;
        link[3] = TRB_SET_TYPE(TRB_LINK) | (1u << 1) /* Toggle Cycle */ | d->int_cycle;
        d->int_enq = 0;
        d->int_cycle ^= 1;
    }
    arch_dsb();
    doorbell(d->slot, d->dci);
    arch_dsb();
}

/* Apply the Ctrl modifier to a decoded character, producing the terminal control code:
 * Ctrl-A..Z -> 0x01..0x1a (so Ctrl-C = 0x03 ETX, the tty's VINTR), Ctrl-@/[/\/]/^/_ ->
 * 0x00..0x1f, Ctrl-Space -> NUL, Ctrl-? -> DEL (0x7f). Other keys pass through. */
static char ctrl_char(char c) {
    unsigned char u = (unsigned char)c;
    if (u >= 'a' && u <= 'z') u -= 32;                  /* fold to the uppercase range */
    if (u >= '@' && u <= '_') return (char)(u & 0x1f);  /* @A..Z[\]^_ -> 0x00..0x1f */
    if (u == ' ')  return 0;                            /* Ctrl-Space = NUL */
    if (u == '?')  return 0x7f;                         /* Ctrl-? = DEL */
    return c;
}

/* Decode a keyboard boot report and feed newly-pressed keys to the tty. */
static void process_kbd_report(struct usb_dev *d, const uint8_t *rpt) {
    int shift = (rpt[0] & 0x22) != 0;             /* L/R Shift modifier bits */
    int ctrl  = (rpt[0] & 0x11) != 0;             /* L/R Ctrl modifier bits */
    for (int i = 2; i < 8; i++) {
        uint8_t kc = rpt[i];
        if (!kc) continue;
        int was = 0;
        for (int j = 0; j < 6; j++) if (d->prev_keys[j] == kc) was = 1;
        if (was) continue;                        /* still held -- not a new press */
        /* Lock keys: toggle the SOFTWARE state only (gates numpad digits / letter
         * case); the physical LED stays at its enumeration-time state. HW-PROVEN
         * (v0.4.192, serial-captured): with multi-arm keeping INT_RING_BUFS
         * interrupt-IN transfers armed through the VL805 TT, a runtime SET_REPORT
         * to this LS keyboard STALLs (cc=6) and the recovery retry times out
         * (cc=-1), wedging the device -- the v0.4.185 kill resurfaced. The
         * endpoint-aware dispatch (v0.4.186) is not sufficient with a full ring.
         * A safe runtime LED needs Stop-Endpoint around the SET_REPORT (backlog).
         * The /proc/xhci.led poke remains as an explicitly-invoked diagnostic. */
        if (kc == 0x53) { d->num_lock = !d->num_lock; continue; }    /* Num Lock    */
        if (kc == 0x39) { d->caps_lock = !d->caps_lock; continue; }  /* Caps Lock   */
        if (kc == 0x47) { d->scroll_lock = !d->scroll_lock; continue; } /* Scroll Lock */
        char ch = hid_to_ascii(d, kc, shift);
        if (!ch) continue;
        if (ctrl) ch = ctrl_char(ch);             /* Ctrl-C -> 0x03, etc. */
        g_key_events++;
        if (g_hid_debug)
            printf("[xhci-kbd] key=0x%02x '%c' (0x%02x)\n", kc,
                   (ch >= 32 && ch < 127) ? ch : '?', (unsigned char)ch);
        /* Arm typematic for the newest press (a later press steals the repeat,
         * matching standard keyboard behaviour). */
        d->rep_kc = kc; d->rep_ch = ch;
        d->rep_deadline = mono_deadline_ms(KBD_REPEAT_DELAY_MS);
        d->rep_active = 1;
        seL4_SetMR(0, (seL4_Word)(unsigned char)ch);
        seL4_Call(serial_ep.cptr, seL4_MessageInfo_new(SER_KEY_PUSH, 0, 0, 1));
    }
    /* Disarm typematic when the armed key is no longer held in this report. */
    if (d->rep_active) {
        int held = 0;
        for (int i = 2; i < 8; i++) if (rpt[i] == d->rep_kc) held = 1;
        if (!held) d->rep_active = 0;
    }
    for (int i = 0; i < 6; i++) d->prev_keys[i] = rpt[i + 2];
}

/* Decode a mouse boot report: byte0 = buttons (bit0 L, 1 R, 2 M), byte1 = dx, byte2 =
 * dy (both signed), byte3 = wheel (if present). Accumulate into the system-mouse state
 * (the Task 4 consumer, read via /proc/mouse) and log motion/clicks. */
static void process_mouse_report(struct usb_dev *d, const uint8_t *rpt) {
    uint8_t btn = rpt[0];
    int dx = (int8_t)rpt[1], dy = (int8_t)rpt[2];
    int wheel = (d->rpt_len > 3) ? (int8_t)rpt[3] : 0;
    if (dx || dy || wheel || btn != d->prev_btn) {
        int nx = g_mouse_x + dx, ny = g_mouse_y + dy;
        if (nx < 0) nx = 0; else if (nx > MOUSE_X_MAX) nx = MOUSE_X_MAX;
        if (ny < 0) ny = 0; else if (ny > MOUSE_Y_MAX) ny = MOUSE_Y_MAX;
        g_mouse_x = nx; g_mouse_y = ny;
        g_mouse_btn = btn;
        g_mouse_events++;
        if (g_hid_debug) {
            g_mouse_events_logged++;
            printf("[xhci-mouse] x=%d y=%d dx=%d dy=%d wheel=%d btn=0x%x\n",
                   nx, ny, dx, dy, wheel, btn);
        }
    }
    d->prev_btn = btn;
}

/* Deliver one received boot report (snapshot) to the right decoder. */
static void process_report(struct usb_dev *d, const uint8_t *rpt) {
    if (d->kind == USB_MOUSE) process_mouse_report(d, rpt);
    else                      process_kbd_report(d, rpt);
}

/* Disarm typematic on every device (dead-man: KBD_REPEAT_MAX_RUN / port change --
 * a keyboard that dies mid-press never sends its release, so repeat would run
 * forever and the echo/scroll storm saturates core 0; v0.4.197). */
static void typematic_disarm_all(const char *why) {
    for (int i = 0; i < MAX_USB_DEV; i++) {
        struct usb_dev *d = &g_devs[i];
        if (d->in_use && d->rep_active) {
            d->rep_active = 0;
            printf("[xhci-kbd] typematic disarmed (%s)\n", why);
        }
    }
}

/* Event-dispatcher hook (see evt_dispatch): a transfer on the device's interrupt-IN
 * endpoint completed. v0.4.192 multi-arm: completions arrive FIFO in the order the
 * buffers were armed, so the report is in buffer (int_proc % INT_RING_BUFS). Snapshot
 * it, RE-ARM that buffer immediately (keeping the ring full so the keyboard is never
 * unpolled), then decode -- even if the decode blocks on the echo, (N-1) transfers
 * stay armed and the controller keeps polling the keyboard. */
static int kbd_try_deliver(uint32_t slot, uint32_t ep, uint32_t cc) {
    for (int i = 0; i < MAX_USB_DEV; i++) {
        struct usb_dev *d = &g_devs[i];
        if (d->in_use && d->kind != USB_NONE && d->kind != USB_HUB
            && d->slot == slot && d->dci == ep) {
            uint32_t bi = d->int_proc % INT_RING_BUFS;
            /* v0.4.204: an error completion carries NO report. Decoding the
             * stale buffer replays old keys as input; never do that. Count it,
             * advance + re-arm (a doorbell on a halted EP is ignored), and PARK
             * the endpoint if errors storm -- a parked keyboard needs a
             * replug/port reset, but it cannot eat core 0. */
            if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) {
                g_int_err_events++;
                if (g_int_err_events <= 3 || (g_int_err_events % 1000) == 0)
                    printf("[xhci] int-IN err cc=%u slot=%u ep=%u total=%u\n",
                           cc, slot, ep, g_int_err_events);
                d->int_proc++;
                if (g_int_err_events < INT_ERR_PARK_THRESHOLD) {
                    arm_int_buf(d, bi);
                } else if (g_int_err_events == INT_ERR_PARK_THRESHOLD) {
                    typematic_disarm_all("int-IN error storm: endpoint parked");
                    printf("[xhci] int-IN PARKED slot=%u ep=%u (err storm)\n", slot, ep);
                }
                return 1;
            }
            volatile uint8_t *src = d->rpt + bi * RPT_STRIDE;
            uint8_t snap[8];
            for (int k = 0; k < 8; k++) snap[k] = src[k];
            d->int_proc++;
            arm_int_buf(d, bi);          /* replenish this buffer -> ring stays full */
            d->rep_run = 0;              /* real report -> reset the dead-man run */
            process_report(d, snap);
            return 1;
        }
    }
    return 0;
}

/* Read the config descriptor, find the HID boot interface (interface protocol 1 =
 * keyboard, 2 = mouse) and its interrupt-IN endpoint, SET_CONFIGURATION, Configure
 * Endpoint, SET_PROTOCOL(boot)/SET_IDLE, and arm the first interrupt transfer. Fills
 * d->kind. Returns 0 with the device armed (and xhci_kbd_ok set) on success. */
static int setup_hid(struct usb_dev *d) {
    uint64_t buf_pa;
    volatile uint8_t *buf = (volatile uint8_t *)dma_page(&buf_pa);
    if (!buf) return -1;
    int cc = control_transfer(d, 0x80, 6, (2u << 8), 0, 9, buf_pa, 1);    /* config hdr */
    if (cc != CC_SUCCESS) return -1;
    uint16_t total = buf[2] | (buf[3] << 8);
    uint8_t cfg_val = buf[5];
    if (total > 4096) total = 4096;
    cc = control_transfer(d, 0x80, 6, (2u << 8), 0, total, buf_pa, 1);    /* full config */
    if (cc != CC_SUCCESS) return -1;

    int iface = -1, ep_addr = -1, ep_mps = 8, ep_interval = 0, in_hid = 0, proto = 0;
    for (int i = 0; i + 2 <= total; ) {
        uint8_t len = buf[i], type = buf[i + 1];
        if (len == 0) break;
        if (type == 4) {                          /* interface descriptor */
            uint8_t icls = buf[i + 5], isub = buf[i + 6], iproto = buf[i + 7];
            /* HID (3), boot subclass (1), protocol 1=keyboard / 2=mouse. */
            in_hid = (icls == 3 && isub == 1 && (iproto == 1 || iproto == 2));
            if (in_hid) { iface = buf[i + 2]; proto = iproto; }
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
    if (ep_addr < 0) { AIOS_LOG_WARN("no HID interrupt-IN endpoint"); return -1; }
    d->kind = (proto == 2) ? USB_MOUSE : USB_KBD;
    d->iface = (uint32_t)iface;
    d->int_mps = (uint32_t)ep_mps;
    d->dci = (uint32_t)((ep_addr & 0xF) * 2 + 1);    /* IN endpoint DCI */
    d->rpt_len = (d->kind == USB_MOUSE) ? 4 : 8;     /* boot report size */
    printf("[xhci] HID %s: slot=%u iface=%d ep=0x%02x mps=%u interval=%d dci=%u\n",
           d->kind == USB_MOUSE ? "mouse" : "keyboard", d->slot, iface, ep_addr,
           d->int_mps, ep_interval, d->dci);

    cc = control_transfer(d, 0x00, 9 /* SET_CONFIGURATION */, cfg_val, 0, 0, 0, 0);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("SET_CONFIG cc=", (unsigned long)cc); return -1; }

    d->int_ring = (volatile uint8_t *)dma_page(&d->int_ring_pa);
    d->rpt = (volatile uint8_t *)dma_page(&d->rpt_pa);
    if (d->kind == USB_KBD)
        d->led_buf = (volatile uint8_t *)dma_page(&d->led_buf_pa);   /* lock-LED Output report */
    if (!d->int_ring || !d->rpt || (d->kind == USB_KBD && !d->led_buf)) return -1;
    d->int_enq = 0; d->int_cycle = 1;

    /* Configure Endpoint: add slot ctx (A0) + the interrupt EP ctx (A[dci]). */
    volatile uint32_t *icc      = (volatile uint32_t *)(d->in_ctx + 0);
    volatile uint32_t *slot_ctx = (volatile uint32_t *)(d->in_ctx + CTX_SZ);
    volatile uint32_t *ep_ctx   = (volatile uint32_t *)(d->in_ctx + (1 + d->dci) * CTX_SZ);
    icc[0] = 0;
    icc[1] = (1u << 0) | (1u << d->dci);
    slot_ctx[0] = (slot_ctx[0] & ~(0x1Fu << 27)) | (d->dci << 27);  /* context entries */
    /* v0.4.205: the Interval field is an EXPONENT -- period = 2^I x 125us. For
     * LS/FS interrupt endpoints bInterval is in 1ms FRAMES, so the largest
     * 2^I x 125us <= bInterval ms is I = floor(log2(bInterval x 8)): 10ms -> I=6
     * (8ms). HS/SS bInterval is already exponent+1. The raw bInterval used to be
     * written here directly, polling this LS keyboard at 2^10 x 125us = 128ms --
     * any press+release inside one window was INVISIBLE (missed keys at speed). */
    uint32_t ivl;
    if (d->speed == 1 || d->speed == 2) {           /* FS / LS: bInterval in ms */
        uint32_t ms8 = (uint32_t)(ep_interval > 0 ? ep_interval : 10) * 8;
        ivl = 31 - (uint32_t)__builtin_clz(ms8);    /* floor(log2(ms x 8)) */
        if (ivl > 10) ivl = 10;
        /* v0.4.221 BCM2711 mitigation: each poll is a VL805 split-transaction
         * DMA burst, and PCIe traffic triggers the 32.4s TLBI/DSB system
         * freezes (project_stall_hunt). 32ms polling = 1/4 the trigger rate of
         * 8ms, still imperceptible for typing (the original bug was 128ms). */
        if (ivl < 8)  ivl = 8;                      /* >= 32ms on this SoC */
    } else {                                        /* HS / SS: exponent + 1 */
        ivl = ep_interval > 0 ? (uint32_t)ep_interval - 1 : 3;
        if (ivl > 15) ivl = 15;
    }
    printf("[xhci] int-IN interval: bInterval=%d speed=%u -> 2^%u x 125us = %uus\n",
           ep_interval, d->speed, ivl, (125u << ivl));
    ep_ctx[0] = ivl << 16;
    ep_ctx[1] = (7u << 3) | (3u << 1) | (d->int_mps << 16);   /* Interrupt IN, CErr=3, MPS */
    ep_ctx[2] = (uint32_t)(d->int_ring_pa | 1);               /* TR dequeue | DCS */
    ep_ctx[3] = (uint32_t)(d->int_ring_pa >> 32);
    ep_ctx[4] = 8 | (d->int_mps << 16);                       /* avg TRB len | max ESIT */
    arch_dsb();
    uint32_t evt[4];
    cc = cmd_submit(d->in_ctx_pa, 0, TRB_CONFIG_EP, d->slot, 0, evt);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("Configure EP cc=", (unsigned long)cc); return -1; }

    control_transfer(d, 0x21, 0x0B /* SET_PROTOCOL */, 0 /* boot */, d->iface, 0, 0, 0);
    control_transfer(d, 0x21, 0x0A /* SET_IDLE */, 0, d->iface, 0, 0, 0);

    /* For a keyboard, apply the initial lock-LED state (Num Lock on) BEFORE arming the
     * interrupt-IN, so this enumeration-time SET_REPORT runs with no report in flight. */
    if (d->kind == USB_KBD)
        set_leds_current(d);

    /* v0.4.192: prime the interrupt-IN ring with INT_RING_BUFS armed transfers so the
     * keyboard is continuously polled even while the driver is busy (see kbd_try_deliver
     * + the INT_RING_BUFS comment). set_leds above ran with nothing armed yet (safe). */
    d->int_proc = 0;
    for (uint32_t i = 0; i < INT_RING_BUFS; i++) arm_int_buf(d, i);
    xhci_kbd_ok = 1;
    printf("[xhci] HID %s ready (polling, %d-deep)\n",
           d->kind == USB_MOUSE ? "mouse" : "keyboard", INT_RING_BUFS);
    return 0;
}

/* First enumerated keyboard (the /proc/xhci LED poke targets it). NULL if none. */
static struct usb_dev *first_kbd(void) {
    for (int i = 0; i < MAX_USB_DEV; i++)
        if (g_devs[i].in_use && g_devs[i].kind == USB_KBD) return &g_devs[i];
    return NULL;
}

/* ---- interrupt (IRQ) mode (Task 2) ----
 * The driver thread can BLOCK on an seL4 IRQ notification instead of busy-polling +
 * Yield, freeing core 0 when idle. Default is POLLING (xhci_irq_mode = 0): the doc keeps
 * polling as the proven fallback until the platform IRQ routing is HW-verified. Flip live
 * via /proc/xhci.irq.1 (and .irq.0 to revert). Routing the controller interrupt to the
 * GIC is platform-specific (plat_pcie_xhci_irq): QEMU wires the xHCI INTx line through the
 * gpex host bridge to a GIC SPI; the RPi4 VL805 raises MSI into the brcmstb root-complex
 * MSI controller. The seL4 IRQHandler + notification are bound in xhci_setup_irq during
 * single-threaded boot. */
static volatile int xhci_irq_mode = 0;       /* 0 = poll (default), 1 = block on IRQ */
static int          xhci_irq_armed = 0;       /* controller INTE enabled yet */
static seL4_CPtr    xhci_irq_ntfn = 0;        /* notification the IRQ signals (0 = unbound) */
static seL4_CPtr    xhci_irq_handler = 0;     /* seL4 IRQHandler cap */
static int          xhci_irq_num = -1;        /* bound GIC IRQ number (diag) */
static volatile uint32_t xhci_irq_count = 0;  /* IRQ wakeups serviced (diag) */

/* Enable controller interrupt generation: interrupter 0 IE (no moderation) + USBCMD.INTE.
 * Done lazily on entering IRQ mode so the poll path stays bit-identical to the proven
 * v0.4.185 behaviour (INTE off). */
static void xhci_irq_enable(void) {
    ir0_w32(IR0_IMOD, 0);                      /* no moderation (lowest latency) */
    ir0_w32(IR0_IMAN, IMAN_IE | IMAN_IP);      /* enable + clear any pending */
    op_w32(XHCI_USBCMD, op_r32(XHCI_USBCMD) | USBCMD_INTE);
    arch_dsb();
}

/* Is the next event-ring TRB unproduced (ring empty)? */
static int evt_ring_empty(void) {
    volatile uint32_t *trb = (volatile uint32_t *)(evt_ring + evt_deq * 16);
    return (trb[3] & TRB_CYCLE) != evt_cycle;
}

/* Bind the controller IRQ to a notification (single-threaded boot context, called from
 * xhci_init). Leaves the controller INTE OFF -- the driver thread enables it only when it
 * enters IRQ mode. No-op (stays in poll mode) if the platform has no usable IRQ routing. */
static void xhci_setup_irq(void) {
    int irq = plat_pcie_xhci_irq();
    if (irq < 0) { AIOS_LOG_INFO("xHCI IRQ routing unavailable -- polling driver"); return; }
    vka_object_t ntfn;
    if (vka_alloc_notification(&vka, &ntfn)) { AIOS_LOG_WARN("xHCI IRQ ntfn alloc failed"); return; }
    cspacepath_t path;
    if (vka_cspace_alloc_path(&vka, &path)) { AIOS_LOG_WARN("xHCI IRQ cspace alloc failed"); return; }
    if (simple_get_IRQ_handler(&simple, irq, path)) {
        AIOS_LOG_WARN_V("xHCI IRQ handler get failed irq=", (unsigned long)irq); return;
    }
    xhci_irq_handler = path.capPtr;
    if (seL4_IRQHandler_SetNotification(xhci_irq_handler, ntfn.cptr)) {
        AIOS_LOG_WARN("xHCI IRQ SetNotification failed"); xhci_irq_handler = 0; return;
    }
    seL4_IRQHandler_Ack(xhci_irq_handler);
    xhci_irq_ntfn = ntfn.cptr;
    xhci_irq_num = irq;
    printf("[xhci] IRQ %d bound (driver polls; enable via /proc/xhci.irq.1)\n", irq);
}

/* USB input driver thread: pump the event ring (evt_dispatch delivers each device's
 * report + re-arms via kbd_try_deliver -- keyboards AND mice), service any LED request
 * posted by the /proc/xhci diagnostic (issuing the SET_REPORT HERE keeps the event ring's
 * single-consumer invariant), then either block on the IRQ (if enabled + bound) or Yield.
 * The IRQ path drains-then-rechecks before blocking, so a report arriving mid-loop still
 * wakes us (latched notification) -- no lost wakeup. */
void xhci_kbd_driver_fn(void *a, void *b, void *c) {
    (void)a; (void)b; (void)c;
    uint32_t e[4];
    while (1) {
        while (evt_dispatch(e, 0 /* no awaited completion */, 0, 0) != DISP_NONE) { }

        uint32_t req = g_led_request;
        if (req & 0x100) {   /* pending LED request from /proc/xhci */
            g_led_request = 0;
            struct usb_dev *d = first_kbd();
            if (d) {
                if (req & 0x200) set_leds_current(d);   /* use current lock state */
                else set_leds(d, (int)(req & 0x7));     /* explicit bitmap */
            }
        }

        /* v0.4.192 typematic: re-emit the held key once its deadline passes (armed
         * in process_kbd_report, disarmed by its release report). Polling-mode only
         * in effect -- in opt-in IRQ mode the loop blocks indefinitely between
         * events, so repeats would stall there (acceptable: polling is the default
         * and the only HW-verified mode). Repeats pause while the driver is blocked
         * in an echo Call and resume after -- never more than one emit per loop. */
        for (int di = 0; di < MAX_USB_DEV; di++) {
            struct usb_dev *d = &g_devs[di];
            if (d->in_use && d->kind == USB_KBD && d->rep_active
                && !mono_before(d->rep_deadline)) {
                /* Dead-man (v0.4.197): too many repeats with no report from this
                 * device = the keyboard likely died mid-press (release lost). */
                if (++d->rep_run > KBD_REPEAT_MAX_RUN) {
                    typematic_disarm_all("dead-man run cap");
                    continue;
                }
                d->rep_deadline = mono_deadline_ms(KBD_REPEAT_RATE_MS);
                seL4_SetMR(0, (seL4_Word)(unsigned char)d->rep_ch);
                seL4_Call(serial_ep.cptr, seL4_MessageInfo_new(SER_KEY_PUSH, 0, 0, 1));
            }
        }

        if (xhci_irq_mode && xhci_irq_ntfn) {
            if (!xhci_irq_armed) { xhci_irq_enable(); xhci_irq_armed = 1; }
            ir0_w32(IR0_IMAN, IMAN_IE | IMAN_IP);       /* clear interrupter pending */
            seL4_IRQHandler_Ack(xhci_irq_handler);      /* re-arm the GIC line */
            arch_dsb();
            if (evt_ring_empty()) {                     /* NAPI-style re-check before block */
                seL4_Wait(xhci_irq_ntfn, NULL);
                xhci_irq_count++;
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
 * directly on a root port. Fills the caller-provided usb_dev d (slot, contexts, EP0 ring).
 * Returns the device class (desc[4]) or -1. */
static int address_and_describe(struct usb_dev *d, uint32_t route, uint32_t root_port,
                                uint32_t speed, uint32_t parent_slot, uint32_t parent_port,
                                uint8_t desc[18]) {
    uint32_t evt[4];
    int cc = cmd_submit(0, 0, TRB_ENABLE_SLOT, 0, 0, evt);
    if (cc != CC_SUCCESS) {
        /* cc=-1 = no Command Completion Event (doorbell/ring/DMA path); cc>0 = a
         * completion code. USBSTS HSE(bit2)/HCE(bit12) flags a host/DMA fault. */
        printf("[xhci] Enable Slot FAILED cc=%d USBSTS=0x%x (HSE=%d HCE=%d)\n",
               cc, op_r32(XHCI_USBSTS), (op_r32(XHCI_USBSTS) >> 2) & 1,
               (op_r32(XHCI_USBSTS) >> 12) & 1);
        return -1;
    }
    d->slot = EVT_SLOT(evt[3]);
    d->speed = speed;

    d->dev_ctx  = (volatile uint8_t *)dma_page(&d->dev_ctx_pa);
    d->in_ctx   = (volatile uint8_t *)dma_page(&d->in_ctx_pa);
    d->ep0_ring = (volatile uint8_t *)dma_page(&d->ep0_ring_pa);
    if (!d->dev_ctx || !d->in_ctx || !d->ep0_ring) return -1;
    d->ep0_enq = 0; d->ep0_cycle = 1;
    dcbaa[d->slot] = d->dev_ctx_pa;

    /* EP0 max packet: SS=512, HS=64 are fixed; LS=8; FS is unknown until we read the
     * descriptor, so start at 8 (the safe minimum) and correct it below. */
    uint32_t mps = (speed == 4) ? 512 : (speed == 3) ? 64 : 8;
    volatile uint32_t *icc      = (volatile uint32_t *)(d->in_ctx + 0);
    volatile uint32_t *slot_ctx = (volatile uint32_t *)(d->in_ctx + CTX_SZ);
    volatile uint32_t *ep0_ctx  = (volatile uint32_t *)(d->in_ctx + 2 * CTX_SZ);
    icc[1] = 0x3;                                  /* Add slot ctx + EP0 ctx */
    slot_ctx[0] = (1u << 27) | (speed << 20) | (route & 0xFFFFF);  /* entries=1, speed, route */
    slot_ctx[1] = (root_port & 0xFF) << 16;        /* root hub port number (1-based) */
    if (parent_slot && (speed == 1 || speed == 2))  /* LS/FS behind a HS hub -> TT info */
        slot_ctx[2] = (parent_slot & 0xFF) | ((parent_port & 0xFF) << 8);
    ep0_ctx[1] = (4u << 3) | (3u << 1) | (mps << 16); /* EP type=Control, CErr=3, MPS */
    ep0_ctx[2] = (uint32_t)(d->ep0_ring_pa | 1);   /* TR dequeue ptr lo | DCS */
    ep0_ctx[3] = (uint32_t)(d->ep0_ring_pa >> 32);
    ep0_ctx[4] = 8;                                /* average TRB length */
    arch_dsb();

    cc = cmd_submit(d->in_ctx_pa, 0, TRB_ADDRESS_DEV, d->slot, 0, evt);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("Address Device cc=", (unsigned long)cc); return -1; }

    uint64_t buf_pa;
    volatile uint8_t *buf = (volatile uint8_t *)dma_page(&buf_pa);
    if (!buf) return -1;
    /* Read the first 8 bytes to learn the real EP0 max packet (FS devices vary 8..64),
     * then correct the EP0 context via Evaluate Context before the full read. */
    cc = control_transfer(d, 0x80, 6, (1u << 8), 0, 8, buf_pa, 1);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("GET_DESCRIPTOR(8) cc=", (unsigned long)cc); return -1; }
    uint32_t real_mps = buf[7];
    if (speed == 1 && real_mps && real_mps != mps) {  /* FS: fix EP0 MPS */
        icc[0] = 0; icc[1] = (1u << 1);               /* A1 = EP0 */
        ep0_ctx[1] = (4u << 3) | (3u << 1) | (real_mps << 16);
        arch_dsb();
        cmd_submit(d->in_ctx_pa, 0, TRB_EVAL_CONTEXT, d->slot, 0, evt);
        mps = real_mps;
    }
    cc = control_transfer(d, 0x80, 6, (1u << 8), 0, 18, buf_pa, 1);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("GET_DESCRIPTOR(18) cc=", (unsigned long)cc); return -1; }
    for (int i = 0; i < 18; i++) desc[i] = buf[i];
    d->mps0 = mps;
    printf("[xhci] device: slot=%u speed=%u mps0=%u VID=%04x PID=%04x class=%u\n",
           d->slot, speed, mps, desc[8] | (desc[9] << 8), desc[10] | (desc[11] << 8), desc[4]);
    return desc[4];
}

/* USB hub (the device on the root port is a hub, class 9 -- e.g. the VL805 internal
 * USB 2.0 hub on the RPi4, behind which every Pi USB-A port hangs). Configure it, power
 * + scan its downstream ports, reset each one with a device, and enumerate every device
 * THROUGH the hub (route string + parent-hub TT) into its own usb_dev. hub is the
 * already-addressed hub device. Returns 0 if at least one HID device was set up. */
static int setup_hub(struct usb_dev *hub, uint32_t hub_root_port, uint32_t hub_speed) {
    (void)hub_speed;
    uint64_t buf_pa;
    volatile uint8_t *buf = (volatile uint8_t *)dma_page(&buf_pa);
    if (!buf) return -1;

    /* Configure the hub: read the config header, SET_CONFIGURATION. */
    int cc = control_transfer(hub, 0x80, 6, (2u << 8), 0, 9, buf_pa, 1);
    if (cc != CC_SUCCESS) return -1;
    uint8_t cfg_val = buf[5];
    cc = control_transfer(hub, 0x00, 9 /* SET_CONFIGURATION */, cfg_val, 0, 0, 0, 0);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN("hub SET_CONFIG failed"); return -1; }

    /* Hub descriptor (class type 0x29) -> number of downstream ports + power-on time. */
    cc = control_transfer(hub, 0xA0, 6, (0x29u << 8), 0, 8, buf_pa, 1);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN("hub descriptor failed"); return -1; }
    uint32_t nports = buf[2];
    uint32_t pwr_good = buf[5];                      /* in 2 ms units */
    /* The per-port "tried" dedup is a 32-bit mask indexed 1..nports, so clamp the
     * device-reported count to 31 (1u << port is UB beyond that). Real hubs have <=7
     * downstream ports; this only guards a malformed descriptor. */
    if (nports > 31) nports = 31;
    hub->kind = USB_HUB;
    printf("[xhci] hub: %u downstream ports\n", nports);

    /* Mark the slot a hub (Hub bit + Number of Ports) so the xHC allocates the TT for
     * LS/FS downstream devices. Evaluate Context with the slot context (A0). */
    volatile uint32_t *icc      = (volatile uint32_t *)(hub->in_ctx + 0);
    volatile uint32_t *slot_ctx = (volatile uint32_t *)(hub->in_ctx + CTX_SZ);
    icc[0] = 0; icc[1] = (1u << 0);                  /* A0 = slot */
    slot_ctx[0] |= (1u << 26);                       /* Hub = 1 */
    slot_ctx[1] = (slot_ctx[1] & 0x00FFFFFF) | (nports << 24); /* Number of Ports */
    arch_dsb();
    uint32_t evt[4];
    cmd_submit(hub->in_ctx_pa, 0, TRB_EVAL_CONTEXT, hub->slot, 0, evt);  /* best effort */

    /* Power every downstream port, then wait power-on-to-power-good. */
    for (uint32_t port = 1; port <= nports; port++)
        control_transfer(hub, 0x23, 3 /* SET_FEATURE */, 8 /* PORT_POWER */, port, 0, 0, 0);
    xhci_mdelay((int)pwr_good * 2 + 100);

    /* Poll the downstream ports for connected devices. Real USB connect-debounce takes
     * ~100s of ms after power-on (QEMU is instant), so scan for up to ~2.5s, trying each
     * connected port once -- a port may appear over time, and there may be more than one
     * device (keyboard AND mouse). Stop early once every connected port is enumerated. */
    uint32_t tried = 0;
    int found = 0;
    for (uint64_t scan = mono_deadline_ms(2500); mono_before(scan); ) {
        for (uint32_t port = 1; port <= nports; port++) {
            if (tried & (1u << port)) continue;
            if (control_transfer(hub, 0xA3 /* GET_STATUS (other) */, 0, 0, port, 4, buf_pa, 1) != CC_SUCCESS)
                continue;
            uint32_t pstat = buf[0] | (buf[1] << 8);
            if (!(pstat & 0x1)) continue;            /* PORT_CONNECTION: no device yet */
            tried |= (1u << port);
            printf("[xhci] hub port %u: device connected (status=0x%x)\n", port, pstat);

            control_transfer(hub, 0x23, 3, 4 /* PORT_RESET */, port, 0, 0, 0);
            for (uint64_t dl = mono_deadline_ms(800); mono_before(dl); ) {
                if (control_transfer(hub, 0xA3, 0, 0, port, 4, buf_pa, 1) != CC_SUCCESS) break;
                if ((buf[2] | (buf[3] << 8)) & 0x10) break;   /* C_PORT_RESET */
                xhci_mdelay(10);
            }
            pstat = buf[0] | (buf[1] << 8);
            control_transfer(hub, 0x23, 1 /* CLEAR_FEATURE */, 20 /* C_PORT_RESET */, port, 0, 0, 0);
            if (!(pstat & 0x2)) { AIOS_LOG_WARN("hub port not enabled after reset"); continue; }
            uint32_t kspeed = (pstat & (1u << 9)) ? 2 /* LS */
                            : (pstat & (1u << 10)) ? 3 /* HS */ : 1 /* FS */;

            struct usb_dev *d = dev_alloc();
            if (!d) break;
            uint8_t kdesc[18];
            int kcls = address_and_describe(d, port, hub_root_port, kspeed, hub->slot, port, kdesc);
            if (kcls < 0) { AIOS_LOG_WARN("downstream device enum failed"); d->in_use = 0; continue; }
            if (kcls == 9) { d->in_use = 0; continue; }   /* nested hub: not supported */
            if (setup_hid(d) == 0) found++;
            else d->in_use = 0;
        }
        /* If a connected port is still un-enumerated, keep scanning; otherwise once we
         * have a device, stop (do not burn the whole 2.5s when devices are present). */
        int pending = 0;
        for (uint32_t port = 1; port <= nports && !pending; port++) {
            if (tried & (1u << port)) continue;
            if (control_transfer(hub, 0xA3, 0, 0, port, 4, buf_pa, 1) == CC_SUCCESS
                && (buf[0] & 0x1)) pending = 1;
        }
        if (found && !pending) break;
        xhci_mdelay(50);
    }
    if (!found) {
        /* Nothing usable: dump each port's final status (PP bit8 = powered, CCS bit0 =
         * connected) to tell "not plugged into this hub" from a power/timing problem. */
        for (uint32_t port = 1; port <= nports; port++)
            if (control_transfer(hub, 0xA3, 0, 0, port, 4, buf_pa, 1) == CC_SUCCESS)
                printf("[xhci] hub port %u: final status=0x%x\n", port, buf[0] | (buf[1] << 8));
        AIOS_LOG_WARN("no HID device found behind hub");
        return -1;
    }
    return 0;
}

/* Reset the root port, address the device on it into a fresh usb_dev, and either arm it
 * (HID keyboard/mouse) or recurse into it (USB hub -- the RPi4 path: input is behind the
 * VL805 hub). */
static int setup_device(uint32_t p) {
    if (port_reset(p)) return -1;
    uint32_t speed = PORTSC_SPEED(op_r32(XHCI_PORTSC(p)));
    struct usb_dev *d = dev_alloc();
    if (!d) return -1;
    uint8_t desc[18];
    int cls = address_and_describe(d, 0, p + 1, speed, 0, 0, desc);
    if (cls < 0) { d->in_use = 0; return -1; }
    if (cls == 9) return setup_hub(d, p + 1, speed);   /* hub -> enumerate downstream */
    if (setup_hid(d) != 0) { d->in_use = 0; return -1; }
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

    /* Bind the controller IRQ now (single-threaded boot). The driver thread still polls
     * by default; /proc/xhci.irq.1 switches it to blocking on this IRQ (Task 2). */
    xhci_setup_irq();

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

    /* ---- C2/C3: enumerate EVERY connected root port. On the RPi4 the keyboard hangs
     * off the VL805 internal hub on one root port; on QEMU devices may sit directly on
     * several root ports. setup_device addresses each and recurses into hubs. ---- */
    for (uint32_t p = 0; p < max_ports; p++)
        if (op_r32(XHCI_PORTSC(p)) & PORTSC_CCS)
            setup_device(p);
    return 0;
}

/* ================================================================
 * Live diagnostic interface -- /proc/xhci. Inspect the controller + every enumerated
 * device AND poke the lock LEDs from the AIOS shell WITHOUT reflashing -- how the
 * deferred-LED HW regression (v0.4.185) is diagnosed. The SET_REPORT is issued by the
 * DRIVER thread (the event ring's single safe consumer); THIS handler runs on the
 * fs/proc thread and only pokes g_led_request + reads (MMIO/DMA snapshots are read-only).
 *
 *   cat /proc/xhci          controller + port + per-device + LED snapshot
 *   cat /proc/xhci.led.N    request lock LEDs = N (hex: bit0 Num, 1 Caps, 2 Scroll)
 *   cat /proc/xhci.lock     request LEDs = current Num/Caps/Scroll software state
 * Each `cat` is one read, so each command runs once. Numbers hex.
 * ================================================================ */
static uint32_t xdiag_hex(const char *p) {
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

int xhci_diag_cmd(const char *args, char *buf, int bufsize) {
    if (!pcie_xhci_present || !xhci_base)
        return snprintf(buf, bufsize, "xHCI: not present\n");

    if (args[0] == '.') {
        const char *p = args + 1;
        if (p[0] == 'l' && p[1] == 'e' && p[2] == 'd' && p[3] == '.') {
            uint32_t bits = xdiag_hex(p + 4) & 0x7;
            g_led_request = 0x100 | bits;     /* driver thread applies it next idle */
            return snprintf(buf, bufsize,
                "xHCI: LED request 0x%x queued (prev SET_REPORT cc=%d USBSTS=0x%x)\n",
                bits, (int)g_led_last_cc, g_led_last_sts);
        }
        if (p[0] == 'l' && p[1] == 'o' && p[2] == 'c' && p[3] == 'k') {
            g_led_request = 0x100 | 0x200;    /* use current lock state */
            return snprintf(buf, bufsize, "xHCI: LED request (current state) queued\n");
        }
        if (p[0] == 'd' && p[1] == 'e' && p[2] == 'b' && p[3] == 'u'
            && p[4] == 'g' && p[5] == '.') {
            g_hid_debug = (int)(xdiag_hex(p + 6) & 1);
            return snprintf(buf, bufsize, "xHCI: per-event HID logging = %d (key_events=%u)\n",
                            g_hid_debug, g_key_events);
        }
        if (p[0] == 'i' && p[1] == 'r' && p[2] == 'q' && p[3] == '.') {
            uint32_t on = xdiag_hex(p + 4) & 1;
            if (on && xhci_irq_ntfn == 0)
                return snprintf(buf, bufsize, "xHCI: IRQ unavailable (no routing bound) -- staying in poll mode\n");
            xhci_irq_mode = (int)on;          /* driver thread switches on its next loop */
            return snprintf(buf, bufsize, "xHCI: IRQ mode = %u (irq=%d count=%u)\n",
                            on, xhci_irq_num, xhci_irq_count);
        }
    }

    int w = 0;
    uint32_t sts = op_r32(XHCI_USBSTS), cmd = op_r32(XHCI_USBCMD);
    w += snprintf(buf + w, bufsize - w,
        "xHCI diag. cmds: .led.N (bit0 Num, 1 Caps, 2 Scroll)  .lock  .irq.0|1  .debug.0|1\n");
    w += snprintf(buf + w, bufsize - w,
        "USBSTS=0x%x (HCH=%d HSE=%d HCE=%d CNR=%d)  USBCMD=0x%x (RS=%d INTE=%d)\n",
        sts, sts & 1, (sts >> 2) & 1, (sts >> 12) & 1, (sts >> 11) & 1,
        cmd, cmd & 1, (cmd >> 2) & 1);
    w += snprintf(buf + w, bufsize - w,
        "irq: mode=%d bound=%d num=%d count=%u  (poll is the default; .irq.1 to block)\n",
        xhci_irq_mode, xhci_irq_ntfn ? 1 : 0, xhci_irq_num, xhci_irq_count);
    w += snprintf(buf + w, bufsize - w,
        "slots=%u ports=%u  kbd_ok=%d  evt_deq=%u cyc=%u  key_events=%u int_errs=%u  last SET_REPORT cc=%d USBSTS=0x%x\n",
        max_slots, max_ports, xhci_kbd_ok, evt_deq, evt_cycle, g_key_events, g_int_err_events,
        (int)g_led_last_cc, g_led_last_sts);
    for (uint32_t pidx = 0; pidx < max_ports && pidx < 8; pidx++) {
        uint32_t sc = op_r32(XHCI_PORTSC(pidx));
        w += snprintf(buf + w, bufsize - w,
            "port %u: portsc=0x%x ccs=%d ped=%d speed=%u\n",
            pidx + 1, sc, sc & 1, (sc >> 1) & 1, PORTSC_SPEED(sc));
    }
    /* Per-device: kind, slot, interrupt-IN DCI, lock state (kbd), and the slot state
     * from the controller-written output context (slot ctx dword 3 bits [31:27]):
     * 1=Default 2=Addressed 3=Configured. A drop to 0/1 after a SET_REPORT would mean
     * the device re-enumerated (a candidate LED-regression cause). */
    for (int i = 0; i < MAX_USB_DEV; i++) {
        struct usb_dev *d = &g_devs[i];
        if (!d->in_use) continue;
        const char *kn = d->kind == USB_KBD ? "kbd" : d->kind == USB_MOUSE ? "mouse"
                       : d->kind == USB_HUB ? "hub" : "?";
        uint32_t st = d->dev_ctx ? (((volatile uint32_t *)d->dev_ctx)[3] >> 27) & 0x1F : 0;
        w += snprintf(buf + w, bufsize - w, "dev[%d] %s slot=%u dci=%u state=%u", i, kn,
                      d->slot, d->dci, st);
        if (d->kind == USB_KBD)
            w += snprintf(buf + w, bufsize - w, " locks(num=%d caps=%d scroll=%d) led=0x%02x",
                          d->num_lock, d->caps_lock, d->scroll_lock, d->led_buf ? d->led_buf[0] : 0);
        w += snprintf(buf + w, bufsize - w, "\n");
    }
    return w;
}

/* /proc/mouse -- the system mouse state (Task 4 consumer). x/y are the accumulated
 * cursor clamped to a virtual 1920x1080 extent; buttons is the current bitmap (bit0 L,
 * 1 R, 2 M); events counts reports that carried motion or a button change. */
int xhci_mouse_state(char *buf, int bufsize) {
    return snprintf(buf, bufsize, "x=%d y=%d buttons=0x%x events=%u\n",
                    g_mouse_x, g_mouse_y, g_mouse_btn, g_mouse_events);
}

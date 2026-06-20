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
#define PORTSC_CSC        (1u << 17)  /* connect status change (W1C) -- hotplug */
#define PORTSC_PLC        (1u << 22)  /* port link state change (W1C) */

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
#define TRB_DISABLE_SLOT  10
#define TRB_ADDRESS_DEV   11
#define TRB_CONFIG_EP     12
#define TRB_EVAL_CONTEXT  13
#define TRB_RESET_EP      14
#define TRB_STOP_EP       15
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
static int hub_try_deliver(uint32_t slot, uint32_t ep, uint32_t cc);   /* Path B: hub status pipe */

/* v0.4.255: USB-MSC runtime block-I/O coordination. The driver thread (xhci_kbd_driver_fn,
 * defined ABOVE the mass-storage section) sets g_msc_driver_running and services FS-thread
 * block requests via msc_service_request -- forward-declared here. */
static volatile int g_msc_driver_running;
static void msc_service_request(void);

/* v0.4.255 Path A (runtime hotplug mount): MSC state lives here, ABOVE device_teardown and
 * the driver loop which both touch it (it is SET by setup_msc far below). g_msc_mount_pending
 * flags an MSC drive that enumerated WHILE the driver thread is running (a hotplugged drive,
 * not the boot drive aios_root.c mounts) -- the driver loop mounts it at top level via
 * msc_runtime_mount. g_msc_mount_inline forces usb_blk_read/write to transfer DIRECTLY for the
 * duration of that mount: the driver thread runs the mount itself, so it must NOT post to
 * g_msc_req and spin-wait for itself (deadlock). g_msc_automount gates it (/proc/xhci.automount.0
 * leaves a hotplugged drive as a raw block device). */
struct usb_dev;
int                     xhci_msc_ok = 0;      /* a USB mass-storage LUN is enumerated + live */
static struct usb_dev  *g_msc_dev;            /* the live MSC device (block backend target) */
static volatile int     g_msc_mount_pending;  /* runtime-enumerated MSC awaits mount */
static volatile int     g_msc_mount_inline;   /* mount in progress -> usb_blk_* transfer direct */
static volatile int     g_msc_automount = 1;  /* /proc/xhci.automount.0 to leave as block dev */
static void msc_runtime_mount(void);

/* ---- command ring + event ring producer/consumer state ---- */
static uint32_t cmd_enq = 0, cmd_cycle = 1;   /* command ring (producer) */
static uint32_t evt_deq = 0, evt_cycle = 1;   /* event ring (consumer) */
static volatile uint32_t g_port_change;       /* v0.4.253: a port-status event is pending
                                               * (set in evt_dispatch, drained by the driver
                                               * thread for hotswap reconcile) */
/* Path B (v0.4.256): a hub interrupt-IN status-change transfer arrived -- a DOWNSTREAM port
 * changed (a hub absorbs downstream changes and reports them on its own interrupt pipe, not as
 * a root Port Status Change Event). Set in evt_dispatch (snapshot/flag only), drained at TOP
 * LEVEL by the driver thread so enumeration never runs nested in a command wait. g_hub_hotplug
 * gates arming the pipe (HW-only-verifiable; default-state set after the QEMU testability probe). */
static volatile uint32_t g_hub_change;
/* Default ON -- HW-VERIFIED (build 2503, real VL805, 2026-06-16). The keyboard rides the SAME
 * hub, and the feared wedge (Configure-Endpoint + CLEAR_FEATURE through the hub TT while the
 * keyboard int-IN is armed) did NOT happen on real silicon: the keyboard was hot-torn-down +
 * re-enumerated behind the hub and kept typing throughout. So the driver thread arms every
 * enumerated hub's status pipe shortly after boot, and a device hotplugged behind the hub is
 * detected at runtime. /proc/xhci.hub.0 is a runtime kill-switch (stops the reconcile; the pipe
 * keeps draining so a keyboard on the hub is unaffected); .hub.1 re-enables. QEMU usb_hub_hotplug
 * 11/11. (The original VL805-TT-wedge concern came from the adversarial review; HW retired it.) */
static volatile int      g_hub_hotplug = 1;
static int               g_hub_armed   = 0;   /* hub status pipes armed (set once on enable) */
static int  setup_hub_int(struct usb_dev *hub);                       /* Path B (defined below) */
static int  hub_enumerate_port(struct usb_dev *hub, uint32_t port);   /* Path B (defined below) */
static void handle_hub_changes(void);                                 /* Path B (defined below) */

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
        /* Path B: a hub status-change transfer (downstream port changed)? kbd_try_deliver
         * excludes hubs, so match it here -- snapshot + flag for the top-level handler, never
         * enumerate inline (this runs nested inside enumeration command waits). */
        if (hub_try_deliver(slot, ep, EVT_CC(e[2]))) return DISP_OTHER;
        kbd_try_deliver(slot, ep, EVT_CC(e[2]));   /* live report -- deliver + re-arm */
        return DISP_OTHER;
    }
    if (type == want_type) {         /* CMD_COMP_EVT for cmd_submit */
        out[0] = e[0]; out[1] = e[1]; out[2] = e[2]; out[3] = e[3];
        return DISP_MATCH;
    }
    /* Port status change = a device reset/connected/disconnected. Any armed typematic
     * may belong to a keyboard that just died mid-press (its release will never arrive)
     * -- disarm rather than risk a repeat runaway (v0.4.197). v0.4.253: flag it so the
     * driver thread reconciles ports at TOP LEVEL (hotswap) -- never enumerate here, this
     * runs nested inside enumeration command waits. */
    if (type == TRB_PORT_STS_EVT) {
        typematic_disarm_all("port change");
        g_port_change = 1;
    }
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
/* v0.4.253: a freelist so pages reclaimed on device teardown (hotswap unplug) are
 * reused -- without it dma_page only bumps and a replug storm exhausts the 2MB pool. */
static uint32_t  dma_freelist[DMA_POOL_PAGES];
static uint32_t  dma_free_count;

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

/* Carve one zeroed page from the low DMA pool; returns vaddr + paddr. Reuses a freed
 * page first (hotswap teardown returns pages here via dma_free), else bumps. */
static void *dma_page(uint64_t *pa_out) {
    uint32_t i;
    if (dma_free_count > 0) {
        i = dma_freelist[--dma_free_count];
    } else if (dma_pool_va && dma_pool_used < DMA_POOL_PAGES) {
        i = dma_pool_used++;
    } else {
        AIOS_LOG_ERROR("xHCI DMA pool exhausted"); return NULL;
    }
    void *va = dma_pool_va + (uint64_t)i * 0x1000;
    for (int k = 0; k < 4096; k++) ((volatile uint8_t *)va)[k] = 0;
    *pa_out = dma_pool_pa + (uint64_t)i * 0x1000;
    return va;
}

/* Return a dma_page()'d page to the freelist (hotswap device teardown). No-op for NULL
 * or a pointer outside the pool. */
static void dma_free(void *va) {
    if (!va || !dma_pool_va) return;
    uint64_t off = (uint64_t)((uint8_t *)va - dma_pool_va);
    if (off >= (uint64_t)DMA_POOL_PAGES * 0x1000 || (off & 0xFFF)) return;
    uint32_t i = (uint32_t)(off >> 12);
    if (dma_free_count < DMA_POOL_PAGES) dma_freelist[dma_free_count++] = i;
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
enum { USB_NONE = 0, USB_KBD = 1, USB_MOUSE = 2, USB_MSC = 8, USB_HUB = 9 };
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
    uint32_t root_port;                             /* 0-based root port (hotswap reconcile) */
    uint32_t parent_slot;                           /* Path B: parent hub slot (0 = root device) */
    uint32_t parent_port;                           /* Path B: 1-based port on the parent hub */
    uint8_t  hub_nports;                            /* Path B: downstream port count (hubs) */
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
    /* v0.4.255: USB Mass Storage (BOT/SCSI) -- two BULK endpoints + capacity.
     * The bulk rings are 1 page each (256 TRBs, Link-wrap at 255 like the int ring);
     * msc_buf is a 4KB DMA scratch for the 31-byte CBW, the 13-byte CSW, and small
     * data phases (INQUIRY/READ_CAPACITY). Block READ/WRITE data uses the caller buf. */
    volatile uint8_t *bo_ring; uint64_t bo_ring_pa; uint32_t bo_enq, bo_cycle, bo_dci; /* bulk OUT */
    volatile uint8_t *bi_ring; uint64_t bi_ring_pa; uint32_t bi_enq, bi_cycle, bi_dci; /* bulk IN  */
    volatile uint8_t *msc_buf; uint64_t msc_buf_pa;
    uint64_t msc_nsectors;     /* total logical blocks (READ_CAPACITY last LBA + 1) */
    uint32_t msc_blocksize;    /* bytes per logical block (usually 512)             */
    uint32_t msc_tag;          /* monotonic CBW tag                                 */
    uint8_t  msc_qemu;         /* INQUIRY product is the QEMU emulated disk -> safe to
                                * run the destructive WRITE(10) self-test on it      */
    volatile uint8_t *msc_io;  uint64_t msc_io_pa;   /* block-I/O DMA bounce buffer (1 page) */
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
        d->root_port = 0xFFFFFFFFu;   /* v0.4.253: "not a direct root-port device" --
                                       * setup_device sets the real port; hub-downstream
                                       * devices keep this sentinel so the root-port
                                       * reconcile never mistakenly tears them down */
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

int xhci_kbd_ok = 0;               /* a keyboard enumerated at boot (diag) */
/* v0.4.255 Path A: the controller is up -> boot_services spawns the polling driver thread
 * UNCONDITIONALLY (even with no boot device), so the event ring is drained and a device
 * HOTPLUGGED after boot is detected. Previously the thread spawned only if a keyboard
 * enumerated at boot, so an insert-after-boot drive was never noticed. */
int xhci_running = 0;

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

/* v0.4.253/254: drive the lock LEDs at RUNTIME (interrupt-IN endpoint already armed).
 *
 * A bare SET_REPORT on EP0 STALLs (cc=6) on the low-speed keyboard behind the VL805
 * transaction translator while interrupt-IN transfers are armed (HW-PROVEN, serial-
 * captured v0.4.192), wedging the device -- which is why the lock keys are SOFTWARE-
 * ONLY. The remedy attempted below: quiesce the interrupt endpoint (Stop Endpoint),
 * SET_REPORT on EP0, then re-establish the interrupt ring and restart it.
 *
 * !! HW RESULT 2026-06-15 (v0.4.254, real Pi via /proc/xhci.led poke): the LED part
 *    WORKS (cat /proc/xhci.led.0 turned the LEDs off -> Stop-Endpoint + SET_REPORT is
 *    fine) but the interrupt-ring RESUME below is WRONG: the keyboard then emitted a
 *    stuck repeated key ('r') and went DEAD. So the re-prime re-delivers a STALE report
 *    and/or the int_enq/int_proc/int_cycle re-sync does not match the HW dequeue after
 *    the Stop. A reboot recovers (the risk stayed contained to the explicit poke).
 *
 * Because getting the ring-resume right needs a real-HW iteration loop with serial
 * capture (each wrong attempt wedges the keyboard -> power-cycle) -- not something that
 * can be done blind -- the wedging sequence is DISABLED (#if 0). The runtime poke is now
 * a SAFE no-op; the lock keys stay software-only (the documented backlog state). Fix
 * direction for that session is in the #if 0 below + docs/NEXT_20260615g. */
static void set_leds_runtime(struct usb_dev *d, int bits) {
    if (!d->led_buf) { g_led_last_cc = 0xFFFFFFFFu; return; }
    /* v0.4.276 LED runtime (serial-capture HW iteration): Stop the interrupt-IN EP (quiesce the
     * LS keyboard behind the VL805 TT so the EP0 SET_REPORT does not contend with armed int-IN
     * transfers -> the bare-SET_REPORT STALL), set the LED, then RESUME the int ring from the
     * dequeue pointer the controller wrote into the OUTPUT device context during the Stop (NOT a
     * forced ring restart, which desynced the cycle bit + wedged). Doorbell to restart the EP.
     * All steps print to serial ([led-rt]) so a wedge is diagnosable. Keypress-wiring stays OFF
     * until the /proc/xhci.led poke proves this path. */
    uint32_t evt[4];
    int sc = cmd_submit(0, 0, TRB_STOP_EP, d->slot, d->dci, evt);   /* quiesce int-IN */
    set_leds(d, bits);                                              /* EP0 SET_REPORT; sets g_led_last_cc */
    int rc = (int)g_led_last_cc;
    /* OUTPUT device context: slot ctx @0, EP ctx for dci @ dci*CTX_SZ; ep[2]=deq_lo|DCS, ep[3]=deq_hi */
    volatile uint32_t *oep = (volatile uint32_t *)(d->dev_ctx + (uint32_t)d->dci * CTX_SZ);
    uint64_t hw_deq = ((uint64_t)oep[3] << 32) | (oep[2] & ~0xFull);
    uint32_t dcs    = oep[2] & 1u;
    long long idx   = (long long)(((int64_t)hw_deq - (int64_t)d->int_ring_pa) / 16);
    int dq = cmd_submit(hw_deq | dcs, 0, TRB_SET_TR_DEQ, d->slot, d->dci, evt);
    doorbell(d->slot, d->dci);                                      /* Stopped -> Running from the dequeue */
    printf("[led-rt] stop=%d setrep=%d hwdeq=0x%llx dcs=%u idx=%lld setdeq=%d enq=%u cyc=%u\n",
           sc, rc, (unsigned long long)hw_deq, dcs, idx, dq, d->int_enq, d->int_cycle);
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

/* ---- hotswap (v0.4.253): runtime device plug/unplug re-enumeration ----
 * evt_dispatch sets g_port_change on a Port Status Change Event; the driver thread (the
 * single event-ring consumer) drains it at TOP LEVEL -- never inline in evt_dispatch,
 * which runs nested inside enumeration command waits (that would re-enter). On a change
 * we re-scan every root port and reconcile against g_devs[]: a newly-connected port with
 * no device is enumerated; a vanished device is torn down (slot + DMA reclaimed). Root
 * ports only -- the RPi4 keyboard behind the VL805 hub needs the hub interrupt-IN status
 * pipe (a follow-up; docs/NEXT_20260615e). */
static int setup_device(uint32_t p);        /* fwd: the per-root-port enumerate path */

static struct usb_dev *dev_on_root_port(uint32_t p) {
    for (int i = 0; i < MAX_USB_DEV; i++)
        if (g_devs[i].in_use && g_devs[i].root_port == p) return &g_devs[i];
    return NULL;
}

/* Path B: find a hub-downstream device by (parent hub slot, downstream port). Downstream
 * devices carry root_port=0xFFFFFFFF (the sentinel), so dev_on_root_port never finds them --
 * they are keyed by parent_slot/parent_port instead. */
static struct usb_dev *dev_on_hub_port(struct usb_dev *hub, uint32_t port) {
    for (int i = 0; i < MAX_USB_DEV; i++)
        if (g_devs[i].in_use && g_devs[i].parent_slot == hub->slot && g_devs[i].parent_port == port)
            return &g_devs[i];
    return NULL;
}

/* Tear down a vanished device: stop its key-repeat, Disable Slot, clear its DCBAA entry,
 * reclaim its 6 DMA pages, free the g_devs slot. (The 2 enum SCRATCH pages are a separate
 * pre-existing leak -- follow-up.) */
static void device_teardown(struct usb_dev *d) {
    if (!d || !d->in_use) return;
    if (d->rep_active) typematic_disarm_all("device unplugged");
    if (d->slot) {
        cmd_submit(0, 0, TRB_DISABLE_SLOT, d->slot, 0, NULL);
        if (d->slot <= max_slots) dcbaa[d->slot] = 0;
    }
    dma_free((void *)d->dev_ctx);   dma_free((void *)d->in_ctx);
    dma_free((void *)d->ep0_ring);  dma_free((void *)d->int_ring);
    dma_free((void *)d->rpt);       dma_free((void *)d->led_buf);
    /* v0.4.255 Path A: also reclaim the MSC pages (NULL on a non-MSC device; dma_free no-ops
     * NULL), invalidate the drive-2 block cache, and drop the block-device globals so a replug
     * re-enumerates cleanly. The /mnt/usb VFS entry persists (no umount path) -- usb_blk_read
     * returns -1 while g_msc_dev is NULL, so accessing the unplugged drive fails gracefully; a
     * replug refreshes ext2_usb in place and reads the new drive (cache invalidated). */
    dma_free((void *)d->bo_ring);   dma_free((void *)d->bi_ring);
    dma_free((void *)d->msc_buf);   dma_free((void *)d->msc_io);
    if (d == g_msc_dev) {
        extern void blk_cache_invalidate(int drive);
        blk_cache_invalidate(2);   /* v0.4.256: drop the unplugged drive's cached sectors so a
                                    * DIFFERENT drive swapped into the slot reads fresh, not stale */
        g_msc_dev = 0; xhci_msc_ok = 0; g_msc_mount_pending = 0;
    }
    printf("[xhci] device unplugged: slot=%u port=%u -- torn down\n", d->slot, d->root_port);
    *d = (struct usb_dev){0};       /* frees the slot (in_use = 0) */
}

/* Reconcile every root port against g_devs[] after a port-status event. Idempotent:
 * a connect/reset event that fires while a device is already up just re-acks. */
static void handle_port_changes(void) {
    g_port_change = 0;
    for (uint32_t p = 0; p < max_ports; p++) {
        uint32_t sc = op_r32(XHCI_PORTSC(p));
        int connected = (sc & PORTSC_CCS) != 0;
        /* ACK the W1C change bits for this port (preserve PP; leave the RW bits alone). */
        op_w32(XHCI_PORTSC(p),
               (sc & PORTSC_PP) | (sc & (PORTSC_CSC | PORTSC_PRC | PORTSC_PLC)));
        struct usb_dev *dev = dev_on_root_port(p);
        if (connected && !dev)       setup_device(p);      /* new plug -> enumerate */
        else if (!connected && dev)  device_teardown(dev); /* unplug -> tear down */
    }
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
    /* v0.4.255: from here ON this thread is the SOLE event-ring consumer, so runtime
     * USB-MSC block I/O (FS thread) must post requests to us rather than transfer
     * directly (see usb_blk_read/write + msc_service_request). */
    g_msc_driver_running = 1;
    uint32_t e[4];
    while (1) {
        while (evt_dispatch(e, 0 /* no awaited completion */, 0, 0) != DISP_NONE) { }

        /* v0.4.255: serve a pending USB-MSC block request from the FS thread. */
        msc_service_request();

        /* v0.4.253 hotswap: a port plug/unplug was flagged by evt_dispatch -- reconcile
         * at top level (safe to enumerate / Disable-Slot here; not nested in a cmd wait). */
        if (g_port_change) handle_port_changes();

        /* Path B: when enabled live (/proc/xhci.hub.1) arm every enumerated hub's status pipe
         * ONCE -- on this (the driver) thread, so the Configure-Endpoint + arm touch the event
         * ring safely. Ships disabled (g_hub_hotplug=0) so the keyboard-on-the-hub path on the
         * RPi4 is undisturbed until a serial-capture HW session verifies it. */
        if (g_hub_hotplug && !g_hub_armed) {
            for (int hi = 0; hi < MAX_USB_DEV; hi++)
                if (g_devs[hi].in_use && g_devs[hi].kind == USB_HUB)
                    setup_hub_int(&g_devs[hi]);
            g_hub_armed = 1;
        }

        /* Path B: a hub status-change event flagged a DOWNSTREAM port change -- reconcile the
         * hub's ports (enumerate/teardown) + re-arm at top level. Before the MSC mount check so
         * a drive enumerated behind the hub sets g_msc_mount_pending and mounts this iteration. */
        if (g_hub_change) handle_hub_changes();

        /* v0.4.255 Path A: a runtime-hotplugged MSC drive needs mounting at /mnt/usb. Runs at
         * TOP LEVEL on this (the sole event-ring consumer) thread, so the mount block I/O
         * transfers directly (g_msc_mount_inline) rather than deadlocking on the request queue. */
        if (g_msc_mount_pending) msc_runtime_mount();

        uint32_t req = g_led_request;
        if (req & 0x100) {   /* pending LED request from /proc/xhci */
            g_led_request = 0;
            struct usb_dev *d = first_kbd();
            if (d) {
                /* v0.4.253: use the Stop-Endpoint RUNTIME path (the int ring is armed
                 * now, unlike at enumeration) so an explicit /proc/xhci.led poke can be
                 * HW-tested without the bare-SET_REPORT STALL that wedged the device.
                 * Keypress-driven LEDs stay OFF until this is HW-proven (see set_leds_runtime). */
                int bits = (req & 0x200)
                    ? ((d->num_lock ? 1 : 0) | (d->caps_lock ? 2 : 0) | (d->scroll_lock ? 4 : 0))
                    : (int)(req & 0x7);
                set_leds_runtime(d, bits);
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
/* Path B: a Transfer Event landed on a hub's interrupt-IN status pipe -> a downstream port
 * changed. Snapshot is in hub->rpt (the status-change bitmap, bit N = port N, bit 0 = hub).
 * Flag for the top-level handler; never enumerate here (called from evt_dispatch, nested in
 * command waits). Returns 1 if (slot,ep) belonged to a hub. */
static int hub_try_deliver(uint32_t slot, uint32_t ep, uint32_t cc) {
    for (int i = 0; i < MAX_USB_DEV; i++) {
        struct usb_dev *hd = &g_devs[i];
        if (hd->in_use && hd->kind == USB_HUB && hd->slot == slot && hd->dci == ep) {
            printf("[xhci] HUB INT event slot=%u ep=%u cc=%u bitmap0=0x%02x\n",
                   slot, ep, cc, hd->rpt ? hd->rpt[0] : 0);
            g_hub_change = 1;
            return 1;
        }
    }
    return 0;
}

/* Path B (v0.4.256): arm the hub interrupt-IN status-change pipe so DOWNSTREAM port changes are
 * reported at RUNTIME (the boot scan only polls). Mirrors setup_hid: read the full config
 * descriptor, find the single interrupt-IN endpoint, Configure-Endpoint it, arm one transfer.
 * The hub reuses the usb_dev int-ring fields (a hub has no HID endpoint). Self-contained
 * (uses hub->hub_nports) so it can be called at boot OR runtime (when /proc/xhci.hub.1 enables
 * it). HW-only-verifiable: QEMU's usb-hub is FS, the VL805 is HS -- the interval is speed-aware. */
static int setup_hub_int(struct usb_dev *hub) {
    if (hub->int_ring) return 0;                  /* already armed */
    uint64_t buf_pa;
    volatile uint8_t *buf = (volatile uint8_t *)dma_page(&buf_pa);
    if (!buf) return -1;
    int rc = -1;
    int cc = control_transfer(hub, 0x80, 6, (2u << 8), 0, 9, buf_pa, 1);   /* config header */
    if (cc != CC_SUCCESS) goto done;
    uint16_t total = buf[2] | (buf[3] << 8);
    if (total > 4096) total = 4096;
    cc = control_transfer(hub, 0x80, 6, (2u << 8), 0, total, buf_pa, 1);   /* full config */
    if (cc != CC_SUCCESS) goto done;
    int ep_addr = -1, ep_mps = 1, ep_interval = 0;
    for (int i = 0; i + 2 <= total; ) {
        uint8_t len = buf[i], type = buf[i + 1];
        if (len == 0) break;
        if (type == 5) {                                  /* endpoint descriptor */
            uint8_t addr = buf[i + 2], attr = buf[i + 3];
            if ((addr & 0x80) && (attr & 0x3) == 3) {     /* interrupt IN = status change */
                ep_addr = addr; ep_mps = buf[i + 4] | (buf[i + 5] << 8); ep_interval = buf[i + 6];
                break;
            }
        }
        i += len;
    }
    if (ep_addr < 0) { AIOS_LOG_WARN("hub: no interrupt-IN status endpoint"); goto done; }
    hub->dci     = (uint32_t)((ep_addr & 0xF) * 2 + 1);
    hub->int_mps = (uint32_t)ep_mps;
    hub->rpt_len = (int)((hub->hub_nports + 8) / 8);      /* status bitmap bytes (bit0 = hub) */
    hub->int_ring = (volatile uint8_t *)dma_page(&hub->int_ring_pa);
    hub->rpt      = (volatile uint8_t *)dma_page(&hub->rpt_pa);
    if (!hub->int_ring || !hub->rpt) goto done;
    hub->int_enq = 0; hub->int_cycle = 1; hub->int_proc = 0;

    /* Configure-Endpoint: slot (keep the Hub bit set by the earlier Evaluate Context) + the
     * interrupt-IN status endpoint. The hub has only EP0 + this int EP, so context entries = dci. */
    volatile uint32_t *icc      = (volatile uint32_t *)(hub->in_ctx + 0);
    volatile uint32_t *slot_ctx = (volatile uint32_t *)(hub->in_ctx + CTX_SZ);
    volatile uint32_t *ep_ctx   = (volatile uint32_t *)(hub->in_ctx + (1 + hub->dci) * CTX_SZ);
    icc[0] = 0; icc[1] = (1u << 0) | (1u << hub->dci);
    slot_ctx[0] = (slot_ctx[0] & ~(0x1Fu << 27)) | (hub->dci << 27);   /* context entries = dci */
    /* interval exponent: HS/SS bInterval is exponent+1; FS/LS is in 1ms frames (QEMU hub is FS,
     * the VL805 hub is HS) -- mirror setup_hid so the poll rate is right on both. */
    uint32_t ivl;
    if (hub->speed == 1 || hub->speed == 2) {             /* FS / LS hub: bInterval in ms */
        uint32_t ms8 = (uint32_t)(ep_interval > 0 ? ep_interval : 12) * 8;
        ivl = 31 - (uint32_t)__builtin_clz(ms8);
        if (ivl > 10) ivl = 10;
        if (ivl < 8)  ivl = 8;
    } else {                                              /* HS / SS: exponent + 1 */
        ivl = ep_interval > 0 ? (uint32_t)ep_interval - 1 : 8;
        if (ivl > 15) ivl = 15;
    }
    ep_ctx[0] = ivl << 16;
    ep_ctx[1] = (7u << 3) | (3u << 1) | (hub->int_mps << 16);          /* Interrupt IN, CErr=3, MPS */
    ep_ctx[2] = (uint32_t)(hub->int_ring_pa | 1);
    ep_ctx[3] = (uint32_t)(hub->int_ring_pa >> 32);
    ep_ctx[4] = 8 | (hub->int_mps << 16);
    arch_dsb();
    uint32_t evt[4];
    cc = cmd_submit(hub->in_ctx_pa, 0, TRB_CONFIG_EP, hub->slot, 0, evt);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("hub int Configure EP cc=", (unsigned long)cc); goto done; }

    arm_int_buf(hub, 0);   /* arm one status-change transfer */
    printf("[xhci] hub status pipe armed: ep=0x%02x dci=%u mps=%u bitmap=%d bytes speed=%u\n",
           ep_addr, hub->dci, hub->int_mps, hub->rpt_len, hub->speed);
    rc = 0;
done:
    dma_free((void *)buf);
    return rc;
}

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
    hub->hub_nports = (uint8_t)nports;   /* Path B: remembered so the status pipe can be armed
                                          * later (runtime, on /proc/xhci.hub.1) -- NOT at boot,
                                          * to keep the keyboard-on-the-same-hub path undisturbed. */

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
            /* reset + enumerate this downstream port (HID or MSC). Shared with the runtime
             * hub-change handler so a USB drive enumerates+mounts behind the hub too. */
            if (hub_enumerate_port(hub, port) == 0) found++;
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

/* ============================================================
 * USB Mass Storage (Bulk-Only Transport + SCSI) over xHCI -- v0.4.255
 * Stage 1: enumerate the two bulk endpoints, INQUIRY + READ_CAPACITY.
 * (Block READ(10)/WRITE(10) + blk_cache registration are later stages.)
 * ============================================================ */
/* xhci_msc_ok + g_msc_dev are defined near the top (above device_teardown, which clears them). */

/* v0.4.257: fault injection for the bulk-STALL recovery path. QEMU usb-storage never STALLs
 * a bulk endpoint, so this lets the recovery (Reset Endpoint + ClearFeature + Set TR Dequeue)
 * be exercised + regression-tested on QEMU: when nonzero, the next N MSC data-phase transfers
 * report CC_STALL to bot_scsi AFTER the real transfer has completed -- so the ring/dequeue
 * state stays consistent and recovery's Set TR Dequeue lands on the live cursor (a no-op there),
 * while the recovery commands themselves still run + log. Set via /proc/xhci.stalltest.N;
 * default 0 (inert). On real HW the STALL is genuine (the endpoint really is Halted).
 * v0.4.273 HW NOTE: arming this on real HW is a QEMU-style aid only -- the fake STALL leaves the
 * EP RUNNING, so bot_ep_recover issues Reset-Endpoint on a NON-Halted EP, which the VL805 treats
 * as disruptive + aborts the in-progress enumeration (the drive then re-enumerates clean via the
 * hub reconcile -> msc ok=1). It still HW-proves the recovery commands EXECUTE on silicon (the
 * msc-stall recoveries counter below climbs in /proc/xhci), but TRANSPARENT in-session recovery
 * can only be confirmed by a GENUINE STALL (a natural SuperSpeed first-replug; HW-proven 2026-06-21
 * build 2729: inject=3 -> recoveries=3 + drive re-enumerated healthy 4TB; no natural STALL repro'd). */
static volatile uint32_t g_msc_stall_inject;
/* v0.4.273: persistent count of bulk-STALL recoveries -- incremented each time bot_ep_recover
 * gets past a Reset-Endpoint that did NOT time out (i.e. the controller returned a completion
 * event; for a genuine Halted-EP STALL that completion is success). The runtime
 * USB driver logs recovery to SERIAL only, and g_msc_stall_inject is write-only via /proc,
 * so this counter is the netconsole-observable proof that the recovery path ran on real HW
 * (read inject + recoveries in /proc/xhci: arm inject=N, replug, expect inject=0 recoveries+=N). */
static volatile uint32_t g_msc_stall_recoveries;
/* v0.4.274 Stage 6: result of the read-only multi-sector READ self-test, run once per enumeration
 * (netconsole-observable via /proc/xhci, since runtime USB enumeration logs only to serial). */
static volatile int g_msc_multi_n;    /* sectors in the last multi-sector self-test (0 = not run) */
static volatile int g_msc_multi_ok;   /* 1 = PASS (multi-read sector 0 == a single READ(10) of LBA 0) */

/* Enqueue one NORMAL TRB on a bulk ring, ring the doorbell, and wait (via the event-
 * ring dispatcher) for its Transfer Event on (slot, dci). Returns the completion code
 * (CC_SUCCESS / CC_SHORT_PKT are both fine); -1 on timeout. Mirrors arm_int_buf's
 * Link-wrap at index 255. SINGLE-CONSUMER: safe at enumeration (init thread); runtime
 * block I/O must serialize vs the driver thread (a later stage). */
static int bot_bulk(struct usb_dev *d, int dir_in, uint64_t buf_pa, uint32_t len) {
    volatile uint8_t *ring = dir_in ? d->bi_ring : d->bo_ring;
    uint64_t ring_pa       = dir_in ? d->bi_ring_pa : d->bo_ring_pa;
    uint32_t *enq          = dir_in ? &d->bi_enq : &d->bo_enq;
    uint32_t *cyc          = dir_in ? &d->bi_cycle : &d->bo_cycle;
    uint32_t dci           = dir_in ? d->bi_dci : d->bo_dci;

    volatile uint32_t *trb = (volatile uint32_t *)(ring + *enq * 16);
    trb[0] = (uint32_t)buf_pa;
    trb[1] = (uint32_t)(buf_pa >> 32);
    trb[2] = len;                                  /* TRB Transfer Length */
    trb[3] = TRB_SET_TYPE(TRB_NORMAL) | (1u << 5) /* IOC */ | *cyc;
    if (++(*enq) == 255) {
        volatile uint32_t *link = (volatile uint32_t *)(ring + 255 * 16);
        link[0] = (uint32_t)ring_pa;
        link[1] = (uint32_t)(ring_pa >> 32);
        link[2] = 0;
        link[3] = TRB_SET_TYPE(TRB_LINK) | (1u << 1) /* Toggle Cycle */ | *cyc;
        *enq = 0;
        *cyc ^= 1;
    }
    arch_dsb();
    doorbell(d->slot, dci);
    arch_dsb();
    uint32_t e[4];
    for (uint64_t dl = mono_deadline_ms(2000); mono_before(dl); ) {
        if (evt_dispatch(e, TRB_TRANSFER_EVT, d->slot, dci) == DISP_MATCH)
            return (int)EVT_CC(e[2]);
    }
    return -1;
}

/* v0.4.257: recover a STALLed (Halted) bulk endpoint per the USB Bulk-Only Transport spec
 * (5.3.x / 6.7.x). A bulk STALL (cc=6) halts the endpoint on BOTH the xHC and the device;
 * left alone, EVERY later transfer on that pipe times out (cc=-1, the doorbell on a Halted
 * EP is ignored). HW-PROVEN: a SuperSpeed drive's first replug STALLs the READ_CAPACITY data
 * phase, then the CSW + the next command's CBW read cc=-1 -> "READ_CAPACITY failed" and the
 * drive only enumerates on a second slot (seed docs/NEXT_20260616). Clear the halt on both
 * sides, mirroring ep0_recover's RESET_EP + SET_TR_DEQ for EP0:
 *   - xHC:    Reset Endpoint (Halted -> Stopped), then Set TR Dequeue to the live ring cursor
 *             with the current cycle bit (the failed TRB was already consumed, so *enq points
 *             at the next free slot = where the next bot_bulk will enqueue + ring the doorbell).
 *   - device: ClearFeature(ENDPOINT_HALT) on EP0 so the device un-stalls the pipe.
 * dir_in picks bulk-IN (1) vs bulk-OUT (0). Bounded: if the Reset Endpoint command itself
 * times out the controller is wedged -- skip the rest rather than compound three command/
 * control busy-waits (~3s of core-0 spin during USB churn; see item 4, project_stall_hunt). */
static void bot_ep_recover(struct usb_dev *d, int dir_in) {
    uint32_t dci     = dir_in ? d->bi_dci : d->bo_dci;
    uint64_t ring_pa = dir_in ? d->bi_ring_pa : d->bo_ring_pa;
    uint32_t enq     = dir_in ? d->bi_enq : d->bo_enq;
    uint32_t cyc     = dir_in ? d->bi_cycle : d->bo_cycle;
    /* USB endpoint address from the DCI: ep_num = dci>>1 (OUT dci=2N, IN dci=2N+1, both
     * floor to N); IN sets bit 7. Exact because setup_msc derived the DCI from (addr & 0xF)
     * and USB endpoint numbers are 4-bit. */
    uint8_t  ep_addr = (uint8_t)((dci >> 1) | (dir_in ? 0x80u : 0u));
    uint32_t evt[4];
    if (cmd_submit(0, 0, TRB_RESET_EP, d->slot, dci, evt) < 0) {
        AIOS_LOG_WARN("MSC bulk-EP reset timed out -- controller wedged, skipping recovery");
        return;
    }
    g_msc_stall_recoveries++;   /* v0.4.273: Reset-EP completed (not a controller-wedge timeout) -> recovery proceeds; HW-observable via /proc/xhci */
    /* Device side: clear ENDPOINT_HALT so the device un-stalls the pipe (BOT 5.3.x). The xHC
     * Reset Endpoint above is what un-wedges OUR ring, so this is best-effort. Both this and the
     * Set TR Dequeue below are checked + logged loudly: an HW poll timeout/failure here must be
     * diagnosable, never silent. */
    int crc = control_transfer(d, 0x02, 1 /* CLEAR_FEATURE */, 0 /* ENDPOINT_HALT */, ep_addr, 0, 0, 0);
    if (crc != CC_SUCCESS) AIOS_LOG_WARN_V("MSC ClearFeature(ENDPOINT_HALT) cc=", (unsigned long)crc);
    uint64_t deq = ring_pa + (uint64_t)enq * 16;
    if (cmd_submit(deq | cyc, 0, TRB_SET_TR_DEQ, d->slot, dci, evt) != CC_SUCCESS)
        AIOS_LOG_WARN("MSC Set-TR-Dequeue after STALL recovery failed -- endpoint may stay Stopped");
}

/* One Bulk-Only Transport command: build the 31-byte CBW (sig "USBC") carrying the
 * SCSI CDB, send it on bulk-OUT, run the optional data phase on the indicated bulk
 * endpoint, then read the 13-byte CSW (sig "USBS") on bulk-IN and validate tag+status.
 * msc_buf layout: CBW @0, CSW @64, small SCSI replies @128. For block I/O the caller
 * passes its own data_pa. Returns 0 on success, -1 on any transport/status failure. */
static int bot_scsi(struct usb_dev *d, const uint8_t *cdb, int cdb_len,
                    int dir_in, uint64_t data_pa, uint32_t data_len) {
    volatile uint8_t *cbw = d->msc_buf;
    uint32_t tag = ++d->msc_tag;
    for (int i = 0; i < 31; i++) cbw[i] = 0;
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;       /* "USBC" (LE 0x43425355) */
    cbw[4] = (uint8_t)tag;        cbw[5] = (uint8_t)(tag >> 8);
    cbw[6] = (uint8_t)(tag >> 16); cbw[7] = (uint8_t)(tag >> 24);
    cbw[8] = (uint8_t)data_len;   cbw[9] = (uint8_t)(data_len >> 8);
    cbw[10] = (uint8_t)(data_len >> 16); cbw[11] = (uint8_t)(data_len >> 24);
    cbw[12] = dir_in ? 0x80 : 0x00;                                  /* bmCBWFlags: IN=0x80 */
    cbw[13] = 0;                                                     /* bCBWLUN = 0 */
    cbw[14] = (uint8_t)cdb_len;                                      /* bCBWCBLength */
    for (int i = 0; i < cdb_len && i < 16; i++) cbw[15 + i] = cdb[i];
    arch_dsb();

    int cc = bot_bulk(d, 0, d->msc_buf_pa, 31);                      /* CBW on bulk-OUT */
    if (cc == CC_STALL) {
        /* A CBW-phase STALL: clear the bulk-OUT halt so the NEXT command is not wedged, then
         * fail this one (the TUR / READ_CAPACITY retry loops re-issue it on a clean pipe). */
        AIOS_LOG_WARN("MSC CBW STALL -- clearing bulk-OUT halt");
        bot_ep_recover(d, 0);
        return -1;
    }
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("MSC CBW cc=", (unsigned long)cc); return -1; }
    if (data_len) {                                                 /* data phase */
        cc = bot_bulk(d, dir_in, data_pa, data_len);
        /* v0.4.257 fault injection (QEMU has no real bulk STALL): fake one AFTER the real
         * transfer so the ring stays consistent -- exercises the recovery path below. */
        uint32_t inj = g_msc_stall_inject;   /* snapshot: set from the proc thread -- no RMW underflow */
        if (inj && (cc == CC_SUCCESS || cc == CC_SHORT_PKT)) {
            g_msc_stall_inject = inj - 1;
            cc = CC_STALL;
            AIOS_LOG_WARN("MSC data STALL INJECTED (test)");
        }
        if (cc == CC_STALL) {
            /* BOT 5.3.x: a data-phase STALL is RECOVERABLE -- clear the halt on the data
             * endpoint and STILL read the CSW (the device reports the command status there).
             * The old code returned here, leaving the endpoint Halted -> the CSW read and the
             * next command's CBW then read cc=-1: the failed-replug wedge this fixes. */
            AIOS_LOG_WARN("MSC data STALL -- clearing halt, reading CSW");
            bot_ep_recover(d, dir_in);
        } else if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) {
            AIOS_LOG_WARN_V("MSC data cc=", (unsigned long)cc); return -1;
        }
    }
    volatile uint8_t *csw = d->msc_buf + 64;                        /* CSW on bulk-IN */
    cc = bot_bulk(d, 1, d->msc_buf_pa + 64, 13);
    if (cc == CC_STALL) {
        /* BOT 6.7.x: a STALL on the status phase -- clear the bulk-IN halt + retry the CSW once. */
        AIOS_LOG_WARN("MSC CSW STALL -- clearing halt, retrying CSW");
        bot_ep_recover(d, 1);
        cc = bot_bulk(d, 1, d->msc_buf_pa + 64, 13);
    }
    if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) {
        AIOS_LOG_WARN_V("MSC CSW cc=", (unsigned long)cc); return -1;
    }
    uint32_t csig = csw[0] | (csw[1] << 8) | (csw[2] << 16) | (csw[3] << 24);
    uint32_t ctag = csw[4] | (csw[5] << 8) | (csw[6] << 16) | (csw[7] << 24);
    /* csw[12] != 0 = SCSI command failed (e.g. the first-access Unit Attention that a
     * TEST UNIT READY clears) -- a normal, expected outcome the CALLER handles (TUR
     * retries, others log context), so do not WARN here. Transport errors (bad bulk cc)
     * are WARNed above. */
    if (csig != 0x53425355u || ctag != tag || csw[12] != 0)
        return -1;
    return 0;
}

static int scsi_test_unit_ready(struct usb_dev *d) {
    uint8_t cdb[6] = { 0x00, 0, 0, 0, 0, 0 };
    return bot_scsi(d, cdb, 6, 0, 0, 0);
}
static int scsi_inquiry(struct usb_dev *d) {
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint64_t dpa = d->msc_buf_pa + 128;
    if (bot_scsi(d, cdb, 6, 1, dpa, 36) != 0) return -1;
    volatile uint8_t *r = d->msc_buf + 128;
    char vendor[9], product[17];
    for (int i = 0; i < 8;  i++) vendor[i]  = (char)r[8 + i];  vendor[8]  = 0;
    for (int i = 0; i < 16; i++) product[i] = (char)r[16 + i]; product[16] = 0;
    printf("[xhci] USB MSC INQUIRY: '%s' '%s'\n", vendor, product);
    d->msc_qemu = (vendor[0] == 'Q' && vendor[1] == 'E' && vendor[2] == 'M' && vendor[3] == 'U');
    return 0;
}
/* SCSI READ CAPACITY(16) (opcode 0x9E, service action 0x10): the 64-bit-LBA capacity
 * query. Used as the fallback when READ_CAPACITY(10) saturates its 32-bit last-LBA field
 * (a drive >= 2TB). Reply: bytes 0..7 = last LBA (big-endian 64-bit), 8..11 = block size. */
static int scsi_read_capacity_16(struct usb_dev *d) {
    uint8_t cdb[16] = { 0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0,
                        0, 0, 0, 32, 0, 0 };          /* alloc length = 32 (CDB[10..13] BE) */
    uint64_t dpa = d->msc_buf_pa + 128;
    if (bot_scsi(d, cdb, 16, 1, dpa, 32) != 0) return -1;
    volatile uint8_t *r = d->msc_buf + 128;
    uint64_t last_lba = 0;
    for (int i = 0; i < 8; i++) last_lba = (last_lba << 8) | r[i];     /* big-endian 64-bit */
    uint32_t bsize = ((uint32_t)r[8] << 24) | ((uint32_t)r[9] << 16) |
                     ((uint32_t)r[10] << 8) |  (uint32_t)r[11];
    d->msc_nsectors  = last_lba + 1;
    d->msc_blocksize = bsize ? bsize : 512;
    return 0;
}
static int scsi_read_capacity(struct usb_dev *d) {
    uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint64_t dpa = d->msc_buf_pa + 128;
    if (bot_scsi(d, cdb, 10, 1, dpa, 8) != 0) return -1;
    volatile uint8_t *r = d->msc_buf + 128;
    uint32_t last_lba = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) |
                        ((uint32_t)r[2] << 8)  |  (uint32_t)r[3];      /* big-endian */
    uint32_t bsize    = ((uint32_t)r[4] << 24) | ((uint32_t)r[5] << 16) |
                        ((uint32_t)r[6] << 8)  |  (uint32_t)r[7];
    /* A drive >= 2TB saturates the 32-bit READ_CAPACITY(10) last-LBA at 0xFFFFFFFF; switch
     * to READ_CAPACITY(16) for the true 64-bit count so blocks past 2TB are addressable.
     * Smaller drives never take this branch -- the proven 10-byte path is byte-identical. */
    if (last_lba == 0xFFFFFFFFu && scsi_read_capacity_16(d) == 0)
        return 0;
    d->msc_nsectors  = (uint64_t)last_lba + 1;
    d->msc_blocksize = bsize ? bsize : 512;
    return 0;
}

/* SCSI READ(10): read `count` logical blocks starting at `lba` into data_pa.
 * count*blocksize must fit the caller's buffer; the caller clamps to MSC_DATA_MAX. */
static int scsi_read_10(struct usb_dev *d, uint32_t lba, uint16_t count, uint64_t data_pa) {
    uint8_t cdb[10] = { 0x28, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
        0, (uint8_t)(count >> 8), (uint8_t)count, 0 };
    return bot_scsi(d, cdb, 10, 1, data_pa, (uint32_t)count * d->msc_blocksize);
}

/* SCSI WRITE(10): write `count` logical blocks at `lba` from data_pa. */
static int scsi_write_10(struct usb_dev *d, uint32_t lba, uint16_t count, uint64_t data_pa) {
    uint8_t cdb[10] = { 0x2A, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
        0, (uint8_t)(count >> 8), (uint8_t)count, 0 };
    return bot_scsi(d, cdb, 10, 0, data_pa, (uint32_t)count * d->msc_blocksize);
}

/* SCSI READ(16)/WRITE(16) (opcodes 0x88/0x8A): 64-bit-LBA block transfer for LBAs beyond
 * the READ(10) 2TB ceiling. CDB[2..9] = LBA (big-endian 64-bit), CDB[10..13] = transfer
 * length in blocks (big-endian 32-bit). */
static int scsi_rw16(struct usb_dev *d, int write, uint64_t lba, uint16_t count,
                     uint64_t data_pa) {
    uint8_t cdb[16] = { (uint8_t)(write ? 0x8A : 0x88), 0,
        (uint8_t)(lba >> 56), (uint8_t)(lba >> 48), (uint8_t)(lba >> 40), (uint8_t)(lba >> 32),
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
        0, 0, (uint8_t)(count >> 8), (uint8_t)count, 0, 0 };
    return bot_scsi(d, cdb, 16, write ? 0 : 1, data_pa, (uint32_t)count * d->msc_blocksize);
}

/* Multi-block read/write that picks READ(10)/WRITE(10) for LBAs that fit the 32-bit field
 * (the proven path, unchanged for < 2TB drives) and READ(16)/WRITE(16) above it. Transfers
 * `count` logical blocks (count*blocksize must fit data_pa's buffer); a run that would straddle
 * the 2^32 LBA boundary takes the 64-bit path so the high blocks stay addressable. */
static int scsi_blk_rw(struct usb_dev *d, int write, uint64_t lba, uint16_t count, uint64_t data_pa) {
    if (lba + (uint64_t)count - 1 > 0xFFFFFFFFu)
        return scsi_rw16(d, write, lba, count, data_pa);
    return write ? scsi_write_10(d, (uint32_t)lba, count, data_pa)
                 : scsi_read_10 (d, (uint32_t)lba, count, data_pa);
}

/* Enumerate a USB Mass Storage (class 8 / Bulk-Only 0x50) device: find the two bulk
 * endpoints, configure both EP contexts in one Configure-Endpoint, then INQUIRY +
 * READ_CAPACITY. Returns 0 if a usable LUN is found. */
static int setup_msc(struct usb_dev *d) {
    uint64_t buf_pa;
    volatile uint8_t *buf = (volatile uint8_t *)dma_page(&buf_pa);
    if (!buf) return -1;
    int cc = control_transfer(d, 0x80, 6, (2u << 8), 0, 9, buf_pa, 1);       /* config hdr */
    if (cc != CC_SUCCESS) return -1;
    uint16_t total = buf[2] | (buf[3] << 8);
    uint8_t cfg_val = buf[5];
    if (total > 4096) total = 4096;
    cc = control_transfer(d, 0x80, 6, (2u << 8), 0, total, buf_pa, 1);       /* full config */
    if (cc != CC_SUCCESS) return -1;

    int iface = -1, in_msc = 0, bo_ep = -1, bi_ep = -1;
    uint32_t bo_mps = 512, bi_mps = 512;
    for (int i = 0; i + 2 <= total; ) {
        uint8_t len = buf[i], type = buf[i + 1];
        if (len == 0) break;
        if (type == 4) {                              /* interface descriptor */
            uint8_t icls = buf[i + 5], iproto = buf[i + 7];
            in_msc = (icls == 8 && iproto == 0x50);   /* Mass Storage, Bulk-Only Transport */
            if (in_msc) iface = buf[i + 2];
        } else if (type == 5 && in_msc) {             /* endpoint descriptor */
            uint8_t addr = buf[i + 2], attr = buf[i + 3];
            uint16_t mps = buf[i + 4] | (buf[i + 5] << 8);
            if ((attr & 0x3) == 2) {                  /* bulk */
                if (addr & 0x80) { bi_ep = addr; bi_mps = mps; }
                else             { bo_ep = addr; bo_mps = mps; }
            }
        }
        i += len;
    }
    if (bo_ep < 0 || bi_ep < 0) { AIOS_LOG_WARN("MSC: no bulk endpoint pair"); return -1; }
    d->kind  = USB_MSC;
    d->iface = (uint32_t)iface;
    d->bo_dci = (uint32_t)((bo_ep & 0xF) * 2);        /* OUT endpoint DCI = ep*2     */
    d->bi_dci = (uint32_t)((bi_ep & 0xF) * 2 + 1);    /* IN  endpoint DCI = ep*2 + 1 */
    printf("[xhci] USB MSC: slot=%u iface=%d bulk-out=0x%02x(dci%u) bulk-in=0x%02x(dci%u)\n",
           d->slot, iface, bo_ep, d->bo_dci, bi_ep, d->bi_dci);

    cc = control_transfer(d, 0x00, 9 /* SET_CONFIGURATION */, cfg_val, 0, 0, 0, 0);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("MSC SET_CONFIG cc=", (unsigned long)cc); return -1; }

    d->bo_ring = (volatile uint8_t *)dma_page(&d->bo_ring_pa);
    d->bi_ring = (volatile uint8_t *)dma_page(&d->bi_ring_pa);
    d->msc_buf = (volatile uint8_t *)dma_page(&d->msc_buf_pa);
    d->msc_io  = (volatile uint8_t *)dma_page(&d->msc_io_pa);   /* block-I/O bounce buffer */
    if (!d->bo_ring || !d->bi_ring || !d->msc_buf || !d->msc_io) return -1;
    d->bo_enq = d->bi_enq = 0; d->bo_cycle = d->bi_cycle = 1; d->msc_tag = 0;

    /* Configure BOTH bulk endpoints in one Configure-Endpoint command. */
    uint32_t max_dci = d->bo_dci > d->bi_dci ? d->bo_dci : d->bi_dci;
    volatile uint32_t *icc      = (volatile uint32_t *)(d->in_ctx + 0);
    volatile uint32_t *slot_ctx = (volatile uint32_t *)(d->in_ctx + CTX_SZ);
    icc[0] = 0;
    icc[1] = (1u << 0) | (1u << d->bo_dci) | (1u << d->bi_dci);
    slot_ctx[0] = (slot_ctx[0] & ~(0x1Fu << 27)) | (max_dci << 27);   /* context entries */
    volatile uint32_t *bo_ctx = (volatile uint32_t *)(d->in_ctx + (1 + d->bo_dci) * CTX_SZ);
    bo_ctx[0] = 0;
    bo_ctx[1] = (2u << 3) | (3u << 1) | (bo_mps << 16);  /* EP type 2 = Bulk OUT, CErr=3 */
    bo_ctx[2] = (uint32_t)(d->bo_ring_pa | 1);           /* TR dequeue | DCS */
    bo_ctx[3] = (uint32_t)(d->bo_ring_pa >> 32);
    bo_ctx[4] = bo_mps;
    volatile uint32_t *bi_ctx = (volatile uint32_t *)(d->in_ctx + (1 + d->bi_dci) * CTX_SZ);
    bi_ctx[0] = 0;
    bi_ctx[1] = (6u << 3) | (3u << 1) | (bi_mps << 16);  /* EP type 6 = Bulk IN, CErr=3 */
    bi_ctx[2] = (uint32_t)(d->bi_ring_pa | 1);
    bi_ctx[3] = (uint32_t)(d->bi_ring_pa >> 32);
    bi_ctx[4] = bi_mps;
    arch_dsb();
    uint32_t evt[4];
    cc = cmd_submit(d->in_ctx_pa, 0, TRB_CONFIG_EP, d->slot, 0, evt);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("MSC Configure EP cc=", (unsigned long)cc); return -1; }

    /* TEST UNIT READY -- a freshly attached LUN often reports Not-Ready / Unit-Attention
     * for the first couple of polls; retry briefly before INQUIRY. */
    for (int i = 0; i < 5; i++) {
        if (scsi_test_unit_ready(d) == 0) break;
        xhci_mdelay(100);
    }
    scsi_inquiry(d);
    /* v0.4.257: a STALLed first access (HW: a SuperSpeed drive's first replug) is now recovered
     * in bot_scsi, but the command that STALLed still reports failure -- retry READ_CAPACITY so
     * the FIRST replug enumerates cleanly on a freshly-cleared pipe (mirrors the TUR retry). */
    int cap_ok = 0;
    for (int i = 0; i < 3; i++) {
        if (scsi_read_capacity(d) == 0) { cap_ok = 1; break; }
        xhci_mdelay(50);
    }
    if (!cap_ok) { AIOS_LOG_WARN("MSC READ_CAPACITY failed"); return -1; }

    unsigned long mb = (unsigned long)((d->msc_nsectors * d->msc_blocksize) >> 20);
    printf("[xhci] USB MSC ready: %lu sectors x %u bytes = %lu MB (slot %u)\n",
           (unsigned long)d->msc_nsectors, d->msc_blocksize, mb, d->slot);
    /* READ-ONLY self-test: READ(10) of LBA 0 validates the bulk data path at
     * enumeration (race-free, single event-ring consumer). Read-only = safe for a
     * real drive. WRITE(10) is exercised only via an explicit diagnostic, never here. */
    if (scsi_read_10(d, 0, 1, d->msc_buf_pa + 128) == 0) {
        volatile uint8_t *s = d->msc_buf + 128;
        printf("[xhci] USB MSC LBA0: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
    } else {
        AIOS_LOG_WARN("MSC READ(10) LBA0 self-test failed");
    }
    /* >2TB drive: also exercise the READ(16) high-LBA path on the LAST block (LBA > 2^32).
     * Read-only (safe on any drive); proves 64-bit-LBA reads work on real HW -- the LBA0
     * test above only covers READ(10). scsi_blk_rw auto-selects READ(16) for the high LBA. */
    if (d->msc_nsectors > 0x100000000ULL) {
        uint64_t last = d->msc_nsectors - 1;
        if (scsi_blk_rw(d, 0, last, 1, d->msc_buf_pa + 128) == 0) {
            volatile uint8_t *s = d->msc_buf + 128;
            printf("[xhci] USB MSC last-LBA(16) @%lu: OK %02x %02x %02x %02x\n",
                   (unsigned long)last, s[0], s[1], s[2], s[3]);
        } else {
            AIOS_LOG_WARN("MSC READ(16) high-LBA self-test failed");
        }
    }
    /* v0.4.274 Stage 6: read-only MULTI-SECTOR self-test -- read up to 8 sectors at LBA 0 in
     * ONE SCSI READ into the msc_io page, then compare sector 0 against a fresh single-sector
     * READ(10). Proves the multi-block backend (usb_blk_read_multi -> scsi_blk_rw count>1) on
     * real HW for ANY drive -- the runtime FS multi-fill path cannot be exercised on a non-ext2
     * drive (e.g. the 4TB Buffalo declines mount). Read-only = safe on a real drive. */
    {
        int mn = (d->msc_nsectors >= 8) ? 8 : (int)d->msc_nsectors;
        volatile uint8_t *m = d->msc_io, *one = d->msc_buf + 128;
        int mok = mn > 0 &&
                  (scsi_blk_rw(d, 0, 0, (uint16_t)mn, d->msc_io_pa) == 0) &&
                  (scsi_read_10(d, 0, 1, d->msc_buf_pa + 128)       == 0);
        if (mok) for (int i = 0; i < 512; i++) if (m[i] != one[i]) { mok = 0; break; }
        g_msc_multi_n = mn; g_msc_multi_ok = mok;   /* /proc/xhci: netconsole-observable on HW */
        printf("[xhci] USB MSC multi-sector READ(%d) self-test: %s\n", mn, mok ? "PASS" : "FAIL");
    }
    /* WRITE(10) self-test -- ONLY on the QEMU emulated disk (DESTRUCTIVE, never a real
     * drive). Write a pattern to LBA 1, read it back, compare. Proves the bulk-OUT data
     * path before the block-device stages wire writes through the filesystem. */
    if (d->msc_qemu && d->msc_blocksize <= 2048) {
        /* pattern in the SEPARATE msc_io page (unused until the driver thread) so the
         * read-back into msc_buf+128 cannot clobber the compare source -- the two would
         * overlap inside msc_buf for any blocksize > 896. */
        volatile uint8_t *pat = d->msc_io, *rb = d->msc_buf + 128;
        for (uint32_t i = 0; i < d->msc_blocksize; i++) pat[i] = (uint8_t)(0xA5 ^ (i & 0xFF));
        int ok = (scsi_write_10(d, 1, 1, d->msc_io_pa)       == 0) &&
                 (scsi_read_10 (d, 1, 1, d->msc_buf_pa + 128) == 0);
        if (ok) for (uint32_t i = 0; i < d->msc_blocksize; i++)
                    if (rb[i] != pat[i]) { ok = 0; break; }
        printf("[xhci] USB MSC WRITE(10) self-test (LBA1, QEMU disk): %s\n", ok ? "PASS" : "FAIL");
    }
    /* Single block-device backend (g_msc_dev). If a drive is ALREADY live, enumerate this one
     * but do NOT make it the /mnt/usb backend -- swapping g_msc_dev under the mounted ext2_usb
     * would corrupt the mount. Multi-drive is out of scope (the first drive wins). */
    if (g_msc_dev && g_msc_dev != d) {
        AIOS_LOG_WARN("second USB MSC device ignored (single-drive; first stays mounted)");
        return 0;
    }
    g_msc_dev = d;
    xhci_msc_ok = 1;
    /* Path A: if the driver thread is already running this is a RUNTIME hotplug (not the boot
     * drive, which aios_root.c mounts directly on the boot thread). Flag it so the driver loop
     * mounts it at top level via msc_runtime_mount (the deadlock-safe path). */
    if (g_msc_driver_running) g_msc_mount_pending = 1;
    return 0;
}

/* ---- USB-MSC block-device backend (blk_cache drive 2) -- v0.4.255 Stage 4 ----
 *
 * THE CONCURRENCY RULE: the xHCI event ring has a SINGLE consumer. During enumeration
 * (boot thread, before the driver loop) that consumer is the boot thread, so block I/O
 * for the mount transfers DIRECTLY. At runtime the FS thread calls usb_blk_read/write
 * but must NOT touch the event ring -- it posts a request to the xHCI driver thread (the
 * sole runtime consumer) via g_msc_req and spin-waits. The driver loop services it with
 * the same scsi_read_10/write_10 path, so keyboard reports still interleave (evt_dispatch
 * delivers them during the bulk wait). One request at a time (the single FS thread). */
static volatile struct {
    int      pending;     /* a request is posted */
    int      write;       /* 1 = write, 0 = read */
    uint64_t lba;         /* 64-bit: >2TB drives use READ(16)/WRITE(16) via scsi_blk_rw */
    int      count;       /* v0.4.274: logical blocks (1 = single sector; up to 8 = read-multi line fill) */
    int      status;      /* result, set by the driver thread (0 = ok) */
    int      done;        /* set by the driver thread on completion */
} g_msc_req;
/* g_msc_driver_running is defined near the top (forward-declared) -- the driver thread
 * that owns it is above this section. */

/* Service a posted block request from the driver loop (single consumer context). */
static void msc_service_request(void) {
    if (!g_msc_req.pending) return;
    struct usb_dev *d = g_msc_dev;
    int rc = -1;
    if (d) rc = scsi_blk_rw(d, g_msc_req.write, g_msc_req.lba,
                            (uint16_t)(g_msc_req.count ? g_msc_req.count : 1), d->msc_io_pa);
    g_msc_req.status = (rc == 0) ? 0 : -1;
    arch_dsb();
    g_msc_req.pending = 0;
    g_msc_req.done = 1;
    arch_dsb();
}

/* blk_cache drive-2 backend: ONE 512-byte sector. Direct transfer during enumeration
 * (boot thread = sole consumer); request-queue + spin-wait at runtime (FS thread). */
int usb_blk_read(uint64_t sector, void *buf) {
    struct usb_dev *d = g_msc_dev;
    if (!d || sector >= d->msc_nsectors) return -1;
    int rc;
    if (!g_msc_driver_running || g_msc_mount_inline) {
        rc = scsi_blk_rw(d, 0, sector, 1, d->msc_io_pa);
    } else {
        g_msc_req.write = 0; g_msc_req.lba = sector; g_msc_req.count = 1;
        g_msc_req.status = -1; g_msc_req.done = 0; arch_dsb();
        g_msc_req.pending = 1; arch_dsb();
        while (!g_msc_req.done) seL4_Yield();
        rc = g_msc_req.status;
    }
    if (rc == 0) { volatile uint8_t *s = d->msc_io; uint8_t *o = buf;
                   for (int i = 0; i < 512; i++) o[i] = s[i]; }
    return rc == 0 ? 0 : -1;
}
int usb_blk_write(uint64_t sector, const void *buf) {
    struct usb_dev *d = g_msc_dev;
    if (!d || sector >= d->msc_nsectors) return -1;
    { volatile uint8_t *s = d->msc_io; const uint8_t *in = buf;
      for (int i = 0; i < 512; i++) s[i] = in[i]; arch_dsb(); }   /* into the DMA buffer first */
    if (!g_msc_driver_running || g_msc_mount_inline)
        return scsi_blk_rw(d, 1, sector, 1, d->msc_io_pa) == 0 ? 0 : -1;
    g_msc_req.write = 1; g_msc_req.lba = sector; g_msc_req.count = 1;
    g_msc_req.status = -1; g_msc_req.done = 0; arch_dsb();
    g_msc_req.pending = 1; arch_dsb();
    while (!g_msc_req.done) seL4_Yield();
    return g_msc_req.status == 0 ? 0 : -1;
}

/* v0.4.274 Stage 6: blk_cache drive-2 multi-block backend -- read `count` (<=8) contiguous
 * sectors in ONE SCSI READ into the msc_io page, then copy out. The cache line-fill calls this
 * with count = BLK_CACHE_LINE_SECTORS (8), so an 8-sector line becomes ONE READ(10) instead of 8
 * -- the Stage 6 perf win. msc_io is exactly one 4KB page = 8 sectors, so each chunk is bounded to
 * 8; a larger request loops. Direct transfer during enumeration/mount (sole consumer), else the
 * g_msc_req queue + spin-wait, exactly like usb_blk_read. */
int usb_blk_read_multi(uint64_t sector, void *buf, int count) {
    struct usb_dev *d = g_msc_dev;
    if (!d || count <= 0) return -1;
    if (sector + (uint64_t)count > d->msc_nsectors) return -1;
    uint8_t *o = buf;
    while (count > 0) {
        int n = count > 8 ? 8 : count;   /* msc_io = one page = 8 sectors */
        int rc;
        if (!g_msc_driver_running || g_msc_mount_inline) {
            rc = scsi_blk_rw(d, 0, sector, (uint16_t)n, d->msc_io_pa);
        } else {
            g_msc_req.write = 0; g_msc_req.lba = sector; g_msc_req.count = n;
            g_msc_req.status = -1; g_msc_req.done = 0; arch_dsb();
            g_msc_req.pending = 1; arch_dsb();
            while (!g_msc_req.done) seL4_Yield();
            rc = g_msc_req.status;
        }
        if (rc != 0) return -1;
        { volatile uint8_t *s = d->msc_io; for (int i = 0; i < n * 512; i++) o[i] = s[i]; }
        sector += (uint64_t)n; o += n * 512; count -= n;
    }
    return 0;
}

/* v0.4.255 Path A: mount a runtime-hotplugged MSC drive at /mnt/usb. Called ONLY from the
 * driver loop at top level (the sole event-ring consumer). g_msc_mount_inline forces the
 * block I/O issued by ext2_init -> blk_cache_read2 -> usb_blk_read to transfer DIRECTLY for
 * the duration -- the driver thread is running the mount, so it cannot post to g_msc_req and
 * wait for itself. usb_msc_mount (boot_fs_init.c) is idempotent on the VFS side (registers the
 * /mnt/usb mount point once; a replug just re-inits ext2_usb in place). A non-ext2 drive logs
 * and stays a raw block device. blk_cache drive 2 is invalidated on unplug (device_teardown
 * -> blk_cache_invalidate, a generation bump), so swapping a DIFFERENT drive into the slot
 * reads fresh -- not the old drive's cached sectors. */
static void msc_runtime_mount(void) {
    extern int usb_msc_mount(void);
    g_msc_mount_pending = 0;
    if (!g_msc_automount) {
        printf("[xhci] USB MSC enumerated; automount OFF (/proc/xhci.automount.1 to enable)\n");
        return;
    }
    g_msc_mount_inline = 1; arch_dsb();
    int rc = usb_msc_mount();
    g_msc_mount_inline = 0; arch_dsb();
    printf("[xhci] USB MSC runtime mount: %s\n",
           rc == 0 ? "mounted at /mnt/usb" : "not mounted (raw/other fs)");
}

/* Reset the root port, address the device on it into a fresh usb_dev, and either arm it
 * (HID keyboard/mouse), enumerate it as mass storage, or recurse into it (USB hub -- the
 * RPi4 path: input is behind the VL805 hub). */
static int setup_device(uint32_t p) {
    if (port_reset(p)) return -1;
    uint32_t speed = PORTSC_SPEED(op_r32(XHCI_PORTSC(p)));
    struct usb_dev *d = dev_alloc();
    if (!d) return -1;
    d->root_port = p;                  /* v0.4.253: for hotswap teardown reconcile */
    uint8_t desc[18];
    int cls = address_and_describe(d, 0, p + 1, speed, 0, 0, desc);
    if (cls < 0) { d->in_use = 0; return -1; }
    if (cls == 9) return setup_hub(d, p + 1, speed);   /* hub -> enumerate downstream */
    if (setup_hid(d) == 0) return 0;                   /* HID keyboard / mouse */
    if (setup_msc(d) == 0) return 0;                   /* v0.4.255: USB mass storage */
    d->in_use = 0;
    return -1;
}

/* Path B: reset + enumerate ONE downstream port of a hub into a fresh usb_dev (the device is
 * known connected). Mirrors setup_device but routes through the hub (route string + parent-hub
 * TT) and dispatches HID AND MSC. Records parent_slot/parent_port so a later unplug is found by
 * dev_on_hub_port. Used by both the boot scan (setup_hub) and the runtime handler. Returns 0 if
 * a device was set up. */
static int hub_enumerate_port(struct usb_dev *hub, uint32_t port) {
    uint64_t buf_pa;
    volatile uint8_t *buf = (volatile uint8_t *)dma_page(&buf_pa);
    if (!buf) return -1;
    control_transfer(hub, 0x23, 3, 4 /* PORT_RESET */, port, 0, 0, 0);
    for (uint64_t dl = mono_deadline_ms(800); mono_before(dl); ) {
        if (control_transfer(hub, 0xA3, 0, 0, port, 4, buf_pa, 1) != CC_SUCCESS) break;
        if ((buf[2] | (buf[3] << 8)) & 0x10) break;   /* C_PORT_RESET */
        xhci_mdelay(10);
    }
    uint32_t pstat = buf[0] | (buf[1] << 8);
    control_transfer(hub, 0x23, 1 /* CLEAR_FEATURE */, 20 /* C_PORT_RESET */, port, 0, 0, 0);
    if (!(pstat & 0x2)) { AIOS_LOG_WARN("hub port not enabled after reset"); dma_free((void *)buf); return -1; }
    uint32_t kspeed = (pstat & (1u << 9)) ? 2 /* LS */
                    : (pstat & (1u << 10)) ? 3 /* HS */ : 1 /* FS */;
    struct usb_dev *d = dev_alloc();
    if (!d) { dma_free((void *)buf); return -1; }
    uint8_t kdesc[18];
    int kcls = address_and_describe(d, port, hub->root_port + 1, kspeed, hub->slot, port, kdesc);
    if (kcls < 0) { AIOS_LOG_WARN("downstream device enum failed"); d->in_use = 0; dma_free((void *)buf); return -1; }
    d->parent_slot = hub->slot;
    d->parent_port = port;
    int rc = -1;
    if (kcls == 9) { d->in_use = 0; }                  /* nested hub: not supported */
    else if (setup_hid(d) == 0) rc = 0;               /* keyboard / mouse */
    else if (setup_msc(d) == 0) rc = 0;               /* mass storage (mounts via g_msc_mount_pending) */
    else d->in_use = 0;
    dma_free((void *)buf);
    return rc;
}

/* Path B: drain a hub status-change interrupt at TOP LEVEL (the driver loop, never inside
 * evt_dispatch -- enumeration nests control transfers). For each hub, read the change bitmap
 * the HW wrote (bit p = downstream port p changed; bit 0 = hub itself), and per flagged port:
 * HUB_GET_STATUS, ACK C_PORT_CONNECTION, reconcile (new connect -> hub_enumerate_port; vanished
 * -> device_teardown). Then RE-ARM the status pipe. g_hub_hotplug gates the reconcile (kill
 * switch); the pipe is always drained + re-armed so a keyboard on the same hub keeps working. */
static void handle_hub_changes(void) {
    g_hub_change = 0;
    for (int i = 0; i < MAX_USB_DEV; i++) {
        struct usb_dev *hub = &g_devs[i];
        if (!hub->in_use || hub->kind != USB_HUB || !hub->rpt || !hub->int_ring) continue;
        /* Snapshot AND clear the change bitmap in one pass, before any control transfer. The
         * status transfer that delivered this bitmap has completed and is NOT re-armed until the
         * end of this hub, so the HW cannot overwrite rpt mid-reconcile; a port that changes
         * during the reconcile is latched by the hub and reported by the next re-armed transfer. */
        uint32_t changed = 0;
        for (int b = 0; b < hub->rpt_len && b < 4; b++) {
            changed |= (uint32_t)hub->rpt[b] << (b * 8);
            hub->rpt[b] = 0;
        }
        arch_dsb();
        if (changed) {
            uint64_t buf_pa;
            volatile uint8_t *buf = (volatile uint8_t *)dma_page(&buf_pa);
            if (buf) {
                for (uint32_t port = 1; port <= 31; port++) {
                    if (!(changed & (1u << port))) continue;
                    if (control_transfer(hub, 0xA3 /* GET_STATUS (other) */, 0, 0, port, 4, buf_pa, 1) != CC_SUCCESS)
                        continue;
                    uint32_t pstat = buf[0] | (buf[1] << 8);
                    control_transfer(hub, 0x23, 1 /* CLEAR_FEATURE */, 16 /* C_PORT_CONNECTION */, port, 0, 0, 0);
                    if (!g_hub_hotplug) continue;     /* kill switch: ACK only, no reconcile */
                    int connected = (pstat & 0x1) != 0;
                    struct usb_dev *dev = dev_on_hub_port(hub, port);
                    printf("[xhci] hub port %u change: %s\n", port, connected ? "connected" : "removed");
                    if (connected && !dev)      hub_enumerate_port(hub, port);
                    else if (!connected && dev) device_teardown(dev);
                }
                dma_free((void *)buf);
            }
        }
        arm_int_buf(hub, 0);   /* re-arm the status pipe for the next change (rpt already cleared) */
    }
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
    xhci_running = 1;   /* controller up -> spawn the polling driver thread (handles hotplug) */
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
        if (p[0] == 'a' && p[1] == 'u' && p[2] == 't' && p[3] == 'o' && p[4] == '.') {
            g_msc_automount = (int)(xdiag_hex(p + 5) & 1);   /* gate runtime hotplug mount */
            return snprintf(buf, bufsize, "xHCI: USB-MSC automount = %d\n", g_msc_automount);
        }
        if (p[0] == 'h' && p[1] == 'u' && p[2] == 'b' && p[3] == '.') {
            g_hub_hotplug = (int)(xdiag_hex(p + 4) & 1);     /* Path B kill switch (default on) */
            return snprintf(buf, bufsize, "xHCI: hub downstream hotplug reconcile = %d\n", g_hub_hotplug);
        }
        if (p[0] == 's' && p[1] == 't' && p[2] == 'a' && p[3] == 'l' && p[4] == 'l'
            && p[5] == 't' && p[6] == 'e' && p[7] == 's' && p[8] == 't' && p[9] == '.') {
            g_msc_stall_inject = xdiag_hex(p + 10) & 0xF;    /* fake N MSC data-phase STALLs (test) */
            return snprintf(buf, bufsize, "xHCI: MSC data-STALL inject = %u\n", g_msc_stall_inject);
        }
    }

    int w = 0;
    uint32_t sts = op_r32(XHCI_USBSTS), cmd = op_r32(XHCI_USBCMD);
    w += snprintf(buf + w, bufsize - w,
        "xHCI diag. cmds: .led.N  .lock  .irq.0|1  .debug.0|1  .auto.0|1  .hub.0|1  .stalltest.N (inject MSC bulk STALL)\n");
    w += snprintf(buf + w, bufsize - w,
        "USBSTS=0x%x (HCH=%d HSE=%d HCE=%d CNR=%d)  USBCMD=0x%x (RS=%d INTE=%d)\n",
        sts, sts & 1, (sts >> 2) & 1, (sts >> 12) & 1, (sts >> 11) & 1,
        cmd, cmd & 1, (cmd >> 2) & 1);
    w += snprintf(buf + w, bufsize - w,
        "irq: mode=%d bound=%d num=%d count=%u  (poll is the default; .irq.1 to block)\n",
        xhci_irq_mode, xhci_irq_ntfn ? 1 : 0, xhci_irq_num, xhci_irq_count);
    { struct usb_dev *md = g_msc_dev;   /* snapshot: teardown may clear it (same core, but clean) */
      w += snprintf(buf + w, bufsize - w,
        "msc: ok=%d automount=%d mount_pending=%d nsectors=%lu blksz=%u\n",
        xhci_msc_ok, g_msc_automount, g_msc_mount_pending,
        md ? (unsigned long)md->msc_nsectors : 0, md ? md->msc_blocksize : 0); }
    w += snprintf(buf + w, bufsize - w,
        "msc-stall: inject=%u recoveries=%u\n", g_msc_stall_inject, g_msc_stall_recoveries);
    w += snprintf(buf + w, bufsize - w,
        "msc-multi: selftest=%s n=%d (Stage 6 multi-sector read)\n",
        g_msc_multi_n ? (g_msc_multi_ok ? "PASS" : "FAIL") : "n/a", g_msc_multi_n);
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
                       : d->kind == USB_HUB ? "hub" : d->kind == USB_MSC ? "msc" : "?";
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

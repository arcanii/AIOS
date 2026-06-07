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

/* Enable a slot, reset + address the device on port p, and read its device
 * descriptor. Returns 0 with dev_slot set on success. */
static int setup_device(uint32_t p) {
    if (port_reset(p)) return -1;
    uint32_t speed = PORTSC_SPEED(op_r32(XHCI_PORTSC(p)));

    uint32_t evt[4];
    int cc = cmd_submit(0, 0, TRB_ENABLE_SLOT, 0, evt);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN("Enable Slot failed"); return -1; }
    dev_slot = EVT_SLOT(evt[3]);

    dev_ctx  = (volatile uint8_t *)dma_page(&dev_ctx_pa);
    in_ctx   = (volatile uint8_t *)dma_page(&in_ctx_pa);
    ep0_ring = (volatile uint8_t *)dma_page(&ep0_ring_pa);
    if (!dev_ctx || !in_ctx || !ep0_ring) return -1;
    ep0_enq = 0; ep0_cycle = 1;
    dcbaa[dev_slot] = dev_ctx_pa;

    /* Input context: Input Control (Add A0|A1) + Slot + EP0 contexts. */
    uint32_t mps = (speed == 4) ? 512 : (speed == 3 || speed == 1) ? 64 : 8;
    volatile uint32_t *icc      = (volatile uint32_t *)(in_ctx + 0);
    volatile uint32_t *slot_ctx = (volatile uint32_t *)(in_ctx + CTX_SZ);
    volatile uint32_t *ep0_ctx  = (volatile uint32_t *)(in_ctx + 2 * CTX_SZ);
    icc[1] = 0x3;                                  /* Add slot ctx + EP0 ctx */
    slot_ctx[0] = (1u << 27) | (speed << 20);      /* context entries=1, speed */
    slot_ctx[1] = (p + 1) << 16;                   /* root hub port number (1-based) */
    ep0_ctx[1] = (4u << 3) | (3u << 1) | (mps << 16); /* EP type=Control, CErr=3, MPS */
    ep0_ctx[2] = (uint32_t)(ep0_ring_pa | 1);      /* TR dequeue ptr lo | DCS */
    ep0_ctx[3] = (uint32_t)(ep0_ring_pa >> 32);
    ep0_ctx[4] = 8;                                /* average TRB length */
    arch_dsb();

    cc = cmd_submit(in_ctx_pa, 0, TRB_ADDRESS_DEV, dev_slot, evt);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("Address Device cc=", (unsigned long)cc); return -1; }
    printf("[xhci] device addressed: slot=%u speed=%u mps0=%u\n", dev_slot, speed, mps);

    /* GET_DESCRIPTOR(device, 18 bytes) -- proves the control path end to end. */
    uint64_t buf_pa;
    volatile uint8_t *buf = (volatile uint8_t *)dma_page(&buf_pa);
    if (!buf) return -1;
    cc = control_transfer(0x80, 6 /* GET_DESCRIPTOR */, (1u << 8) /* DEVICE */, 0, 18, buf_pa, 1);
    if (cc != CC_SUCCESS) { AIOS_LOG_WARN_V("GET_DESCRIPTOR cc=", (unsigned long)cc); return -1; }
    uint16_t vid = buf[8] | (buf[9] << 8);
    uint16_t pid = buf[10] | (buf[11] << 8);
    printf("[xhci] device descriptor: VID=%04x PID=%04x class=%u mps0=%u\n",
           vid, pid, buf[4], buf[7]);

    /* C4: config descriptor -> HID keyboard endpoint -> arm interrupt transfers. */
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

/*
 * pcie_brcmstb.c -- BCM2711 (RPi4) PCIe root complex -- USB HID Phase D.1
 *
 * Layer 1 for RPi4 (docs/DESIGN_USB_HID.md "Phase D findings"). Brings up the
 * brcmstb PCIe link and detects the VIA VL805 xHCI controller over config space.
 *
 * D.1 SCOPE (this file): link bring-up + VL805 config detection. This works
 * WITHOUT the seL4 kernel change, because config access goes through the
 * controller registers (EXT_CFG window at base+0x8000), NOT the outbound MMIO
 * window. It returns -1 (leaving pcie_xhci_present=0, so xhci_init does not run):
 * reaching the xHCI BAR needs the outbound window at CPU 0x6_00000000, which
 * seL4's bcm2711 kernel does not expose (RAM/devices top out under 4GB). That is
 * D.0 (kernel device-region extension) + D.2 (window + BAR), still pending.
 *
 * HW-ONLY: QEMU virt has no brcmstb controller, so this path is exercised only on
 * real hardware. It is UNTESTED until flashed. Bring-up sequence + exact register
 * offsets are from U-Boot/Linux pcie-brcmstb (see the design doc). If the RPi
 * firmware already trained the link (it loads the VL805 firmware at boot), we
 * SKIP the reset to avoid disturbing that state -- and the "link already up" log
 * tells us which case we are in.
 */
#include "aios/root_shared.h"
#include "aios/device_map.h"
#include <sel4/sel4.h>
#include <stdio.h>
#include "arch.h"
#include "aios/mono_wait.h"
#include "aios/hw_info.h"
#include "aios/pcie.h"
#define LOG_MODULE "pcie"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "aios/aios_log.h"

/* Discovered xHCI controller (set only when fully usable -- D.2). */
uint64_t pcie_xhci_bar = 0;
uint64_t pcie_xhci_bar_size = 0;
uint8_t  pcie_xhci_bus = 0, pcie_xhci_dev = 0, pcie_xhci_fn = 0;
int      pcie_xhci_present = 0;

/* brcmstb register offsets (bytes from the controller base 0xFD500000). */
#define RGR1_SW_INIT_1          0x9210
#define   SW_INIT_PERST         0x1         /* 0 = assert PERST#, 1 = deassert */
#define   SW_INIT_BRIDGE        0x2         /* INIT_GENERIC: 1 = assert, 0 = deassert */
#define HARD_DEBUG              0x4204
#define   HARD_DEBUG_SERDES_IDDQ (1u << 27)
#define MISC_CTRL               0x4008
#define   MISC_SCB_ACCESS_EN    0x1000
#define   MISC_CFG_READ_UR      0x2000
#define   MISC_MAX_BURST_MASK   0x300000    /* 0 => 128 bytes on 2711 */
#define   MISC_SCB0_SIZE_MASK   0xf8000000u
#define PCIE_STATUS             0x4068
#define   STATUS_PORT           0x80        /* 1 = root-complex mode */
#define   STATUS_DL_ACTIVE      0x20
#define   STATUS_PHYLINKUP      0x10
#define MISC_REVISION           0x406c
#define RC_BAR1_CONFIG_LO       0x402c
#define RC_BAR2_CONFIG_LO       0x4034
#define RC_BAR2_CONFIG_HI       0x4038
#define RC_BAR3_CONFIG_LO       0x403c
#define EXT_CFG_INDEX           0x9000
#define EXT_CFG_DATA            0x8000
#define RC_CFG_PRIV1_ID_VAL3    0x043c      /* RC class code */
#define RC_BAR2_SIZE_3GB_ENC    17          /* log2(roundup(3GB)=4GB) - 15 */

static volatile uint8_t *reg;

static inline uint32_t rd(uint32_t o) { return *(volatile uint32_t *)(reg + o); }
static inline void wr(uint32_t o, uint32_t v) { *(volatile uint32_t *)(reg + o) = v; }
static inline void setb(uint32_t o, uint32_t m) { wr(o, rd(o) | m); }
static inline void clrb(uint32_t o, uint32_t m) { wr(o, rd(o) & ~m); }
static inline void clrset(uint32_t o, uint32_t c, uint32_t s) { wr(o, (rd(o) & ~c) | s); }
static void mdelay(int ms) { for (uint64_t dl = mono_deadline_ms(ms); mono_before(dl); ) {} }

static int link_up(void) {
    uint32_t s = rd(PCIE_STATUS);
    return (s & STATUS_DL_ACTIVE) && (s & STATUS_PHYLINKUP);
}

/* Config read: bus 0 = RC registers direct; bus >= 1 = EXT_CFG index/data window. */
static uint32_t cfg_rd(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off) {
    if (bus == 0) return rd(off);
    wr(EXT_CFG_INDEX, (bus << 20) | (dev << 15) | (fn << 12));
    arch_dsb();
    return *(volatile uint32_t *)(reg + EXT_CFG_DATA + off);
}

/* Full controller bring-up (only when the firmware did not already train it). */
static void bringup(void) {
    AIOS_LOG_INFO("brcmstb: bring-up (link was down)");
    setb(RGR1_SW_INIT_1, SW_INIT_BRIDGE);   /* assert bridge reset */
    clrb(RGR1_SW_INIT_1, SW_INIT_PERST);    /* assert PERST# */
    mdelay(2);
    clrb(RGR1_SW_INIT_1, SW_INIT_BRIDGE);   /* deassert bridge reset */
    clrb(HARD_DEBUG, HARD_DEBUG_SERDES_IDDQ);
    mdelay(1);
    /* MISC_CTRL: SCB access + UR mode + 128B burst (clear mask) + SCB0 size. */
    clrset(MISC_CTRL, MISC_MAX_BURST_MASK | MISC_SCB0_SIZE_MASK,
           MISC_SCB_ACCESS_EN | MISC_CFG_READ_UR | (RC_BAR2_SIZE_3GB_ENC << 27));
    /* Inbound DMA window RC_BAR2: PCI 0 -> CPU 0 (identity), 3GB. */
    wr(RC_BAR2_CONFIG_LO, 0u | RC_BAR2_SIZE_3GB_ENC);
    wr(RC_BAR2_CONFIG_HI, 0);
    clrb(RC_BAR1_CONFIG_LO, 0x1f);          /* disable GISB window */
    clrb(RC_BAR3_CONFIG_LO, 0x1f);          /* disable SCB window */
    setb(RGR1_SW_INIT_1, SW_INIT_PERST);    /* deassert PERST# -> start link */
    mdelay(100);                            /* PCIe spec post-PERST */
    for (uint64_t dl = mono_deadline_ms(200); mono_before(dl); ) {
        if (link_up()) break;
        mdelay(5);
    }
}

int plat_pcie_init(void) {
    if (!hw_info.has_pcie) { AIOS_LOG_WARN("no PCIe in DTB"); return -1; }
    if (!dev_pcie_vaddr) { AIOS_LOG_WARN("PCIe regs not pre-mapped (prealloc)"); return -1; }
    reg = (volatile uint8_t *)dev_pcie_vaddr;
    printf("[pcie] brcmstb @ 0x%lx rev=0x%04x status=0x%x\n",
           (unsigned long)hw_info.pcie_ecam_paddr, rd(MISC_REVISION) & 0xffff, rd(PCIE_STATUS));

    /* Probe the firmware-state unknown: if the RPi firmware already trained the
     * link, do NOT reset (that could drop the VL805 firmware). Otherwise bring
     * it up ourselves. */
    if (link_up()) {
        AIOS_LOG_INFO("brcmstb: link already up (firmware) -- skipping reset");
    } else {
        bringup();
    }

    uint32_t st = rd(PCIE_STATUS);
    int up = link_up();
    printf("[pcie] PCIE_STATUS=0x%x link=%s mode=%s\n",
           st, up ? "UP" : "DOWN", (st & STATUS_PORT) ? "RC" : "EP");
    if (!up) {
        AIOS_LOG_WARN("brcmstb: link DOWN (no device, or VL805 fw not loaded)");
        return -1;
    }

    /* Tag the RC as a PCIe-PCIe bridge (cosmetic but matches Linux/U-Boot). */
    clrset(RC_CFG_PRIV1_ID_VAL3, 0xffffff, 0x060400);

    /* Detect the VL805 xHCI on bus 1 (one device per bus on brcmstb). */
    uint32_t id = cfg_rd(1, 0, 0, 0x00);
    uint16_t vendor = id & 0xffff;
    if (vendor == 0xffff) {
        AIOS_LOG_WARN("brcmstb: link up but no device on bus 1");
        return -1;
    }
    uint16_t device = (id >> 16) & 0xffff;
    uint32_t cls = cfg_rd(1, 0, 0, 0x08) >> 8;
    printf("[pcie] bus1 dev0: VID=%04x PID=%04x class=%06x\n", vendor, device, cls);

    if (cls == 0x0C0330) {
        printf("[pcie] VL805 xHCI DETECTED -- Phase D.1 link OK.\n");
        printf("[pcie] xHCI BAR/window deferred to D.2 (needs seL4 >4GB MMIO window).\n");
        /* D.2: program BAR0 in the PCI window (0xC0000000+), map CPU side
         * (0x6_00000000+), set pcie_xhci_* + pcie_xhci_present so xhci_init runs.
         * Blocked until seL4 exposes the window. */
    }
    return -1;   /* D.1: link + device verified; no usable xHCI BAR yet (D.2). */
}

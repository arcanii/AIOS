/*
 * pcie_brcmstb.c -- BCM2711 (RPi4) PCIe root complex -- USB HID Phase D.1
 *
 * Layer 1 for RPi4 (docs/DESIGN_USB_HID.md). Brings up the brcmstb PCIe link and
 * detects the VIA VL805 xHCI controller over config space.
 *
 * THE v0.4.183 "CRASH" WAS A RESET-ORDERING BUG, NOT A POWER-GATE. The old code
 * read PCIe-CORE registers (MISC_REVISION 0x406c, PCIE_STATUS 0x4068) in its
 * opening printf -- BEFORE running the reset sequence. Those MISC/core registers
 * (0x4xxx) are only clocked AFTER the bridge reset is deasserted + SERDES IDDQ is
 * cleared, so reading them first raised an SError -> kernel halt. The reset-
 * controller register RGR1_SW_INIT_1 (0x9210) lives in an always-on sub-block and
 * is safe to write while the core is held in reset. THE FIX: run the RGR1 reset
 * dance FIRST (as U-Boot / Linux / Circle pcie-brcmstb do), THEN read the core.
 * No firmware or EEPROM change is needed -- bare-metal Circle brings up Pi4 USB
 * with exactly this reset sequence and NO VC-mailbox power call.
 *
 * VL805 FIRMWARE (HW finding 2026-06-08): after we bring the link up, the VL805
 * trains the PCIe PHY (PCIE_STATUS link=UP) but its config space stays dead --
 * reads return 0xffff/UR for seconds -- because it has NO firmware running (the
 * firmware boot log shows "vl805.bin not found", and this board does NOT self-load
 * the VL805 fw after our PCI reset). The fix: AFTER the link is up, issue the VC
 * mailbox NOTIFY_XHCI_RESET (tag 0x00030058, dev_addr 0x100000) -- the RPi "kernel
 * did the PCI setup first" contract -- so the VC loads the VL805 fw; then config
 * reads return VID=1106. NOTIFY does NOT power the RC (we bring the RC up
 * ourselves) and only works once the link is up.
 *
 * DEBUNKED (do not re-try): (1) a PRE-link-up VC-mailbox "power-on"
 * (SET/GET_DOMAIN_STATE) -- the RC is not power-gated and GET_DOMAIN_STATE returned
 * garbage on this fw; (2) bootloader-EEPROM VL805=1 -- COMPUTE MODULE 4 ONLY (RPi
 * docs), it does nothing on a Pi 4B.
 *
 * SAFETY: the controller read is still an explicit opt-in (PCIE_PROBE_LEVEL,
 * default 0 = no controller MMIO -> a boot that cannot SError) because the
 * corrected sequence is HW-UNVERIFIED (the prior crash earns the caution). Build
 * PCIE_PROBE_LEVEL=1 to probe; if it ever SErrors, reflash disk/sdcard-rpi4.img.
 *
 * D.1 SCOPE: link bring-up + VL805 config detection via the controller registers
 * (EXT_CFG window at base+0x8000). The xHCI BAR (MMIO at CPU 0x6_00000000) needs
 * the seL4 bcm2711 >4GB device window -- that is D.2 (gate 2), still pending; this
 * returns -1 (pcie_xhci_present stays 0, so xhci_init does not run).
 *
 * HW-ONLY: QEMU virt has no brcmstb controller. UNTESTED until flashed. Offsets
 * from U-Boot / Linux / Circle pcie-brcmstb (see the design doc).
 */
#include "aios/root_shared.h"
#include "aios/device_map.h"
#include <sel4/sel4.h>
#include <sel4platsupport/device.h>
#include <stdio.h>
#include "arch.h"
#include "aios/mono_wait.h"
#include "aios/hw_info.h"
#include "aios/pcie.h"
#define LOG_MODULE "pcie"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "aios/aios_log.h"

/* Controller-read opt-in. 0 (default) = no controller MMIO at all (the boot just
 * logs that PCIe probing is disabled -- it cannot SError). 1 = run the brcmstb
 * reset sequence + read the core + detect the VL805. The corrected reset-first
 * ordering is HW-unverified, so this stays opt-in; if level 1 SErrors, reflash the
 * recovery image. Build -DPCIE_PROBE_LEVEL=1 (or edit this line) to probe. */
#ifndef PCIE_PROBE_LEVEL
/* Probe depth (RPi4 only; QEMU uses pcie_ecam.c, unaffected):
 *   0 = no controller MMIO at all (a boot that cannot SError -- recovery default).
 *   1 = D.1 link bring-up + VL805 detect + D.2a BAR program (no BAR read).
 *   2 = D.2: also set pcie_xhci_present -> xhci_init() maps + drives the BAR -> a USB
 *       keyboard types into the shell.
 * Level 2 is HW-VERIFIED (v0.4.184: keyboard works on a real Pi 4) and is the default
 * so a fresh build gets USB input. The bring-up degrades gracefully (link down / no
 * device / unmappable BAR all return -1, no crash); the v0.4.183 SError was the now-
 * fixed reset-ordering bug. Build -DPCIE_PROBE_LEVEL=0 for a guaranteed-no-MMIO image. */
#define PCIE_PROBE_LEVEL 2
#endif

/* Source-B A/B bisect gate (default 0 = full bring-up, no behavior change).
 * Stops the bring-up at a defined point so boot variants isolate which PCIe-side
 * state triggers the 10.8s-quantum TLBI/DSB stalls (project_stall_hunt Source B;
 * quanta fire with ZERO USB devices, so the v0.4.221 keyboard-split-DMA theory is
 * incomplete). Build one kernel per value; each stop prints [pcie] AB-STOP=N on
 * the serial console so the running variant is verifiable from the boot capture.
 *   1 = link trained + outbound window + SSC only. NO VL805 config access, NO
 *       NOTIFY_XHCI_RESET (after our PERST the VL805 has no firmware -- deadest
 *       possible link partner), no BAR, no bus-master.
 *   2 = + VL805 firmware load (mbox) + config-space detect. No BAR, no mem-space,
 *       no bus-master: the device firmware runs but cannot DMA or decode MMIO.
 *   3 = + BAR0 programmed + mem-space + bus-master granted. pcie_xhci_present
 *       stays 0 so xhci_init never runs: nothing resets/starts the xHC, no rings,
 *       no scratchpads -- the device CAN master the bus but was never asked to.
 * (PCIE_PROBE_LEVEL=0 remains the all-off variant: no controller MMIO at all.) */
#ifndef PCIE_AB_STOP
#define PCIE_AB_STOP 0
#endif

/* Discovered xHCI controller (set only when fully usable -- D.2). */
uint64_t pcie_xhci_bar = 0;
uint64_t pcie_xhci_bar_size = 0;
uint8_t  pcie_xhci_bus = 0, pcie_xhci_dev = 0, pcie_xhci_fn = 0;
int      pcie_xhci_present = 0;

#if PCIE_PROBE_LEVEL >= 1
/* =================================================================
 * Controller access (OPT-IN). Everything below touches 0xFD500000 and is compiled
 * only when PCIE_PROBE_LEVEL >= 1 -- a deliberate opt-in (the corrected reset-first
 * sequence is HW-unverified). See the file header for why the v0.4.183 read SError
 * was an ordering bug, not a power-gate.
 * ================================================================= */

/* brcmstb register offsets (bytes from the controller base 0xFD500000). */
#define RGR1_SW_INIT_1          0x9210      /* reset controller -- ALWAYS-ON block */
#define   SW_INIT_PERST         0x1         /* 1 = assert PERST# (hold EP in reset), 0 = deassert (release) */
#define   SW_INIT_BRIDGE        0x2         /* INIT_GENERIC: 1 = assert, 0 = deassert */
#define HARD_DEBUG              0x4204
#define   HARD_DEBUG_SERDES_IDDQ (1u << 27)
#define MISC_CTRL               0x4008
#define   MISC_SCB_ACCESS_EN    0x1000
#define   MISC_CFG_READ_UR      0x2000
#define   MISC_MAX_BURST_MASK   0x300000    /* 0 => 128 bytes on 2711 */
#define   MISC_SCB0_SIZE_MASK   0xf8000000u
#define RC_CONFIG_RETRY_TIMEOUT 0x405c      /* v0.4.213: bound stalled cfg retries */
#define UBUS_TIMEOUT            0x40a8      /* v0.4.213: bound CPU<->PCIe bus stalls */
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
/* RC PCI config header (bus 0 = controller base + offset). The RC is a Type-1
 * (bridge) header: the bridge must forward config to its secondary bus. */
#define RC_COMMAND              0x04        /* PCI_COMMAND (mem + bus master) */
#define RC_PRIMARY_BUS          0x18        /* pri[7:0]|sec[15:8]|sub[23:16] */
/* VL805 endpoint (Type-0) config header, accessed on bus 1 via EXT_CFG. */
#define EP_COMMAND              0x04        /* PCI_COMMAND (mem-space + bus master) */
#define EP_BAR0                 0x10        /* BAR0 -- xHCI MMIO (may be 64-bit) */
#define PCI_CMD_MEM             0x2
#define PCI_CMD_MASTER          0x4
/* Outbound MEM window 0 (CPU->PCI). base in BASE_LIMIT[15:4] (mask 0xfff0),
 * limit in BASE_LIMIT[31:20] (mask 0xfff00000), high bits >> 12 in BASE_HI/
 * LIMIT_HI[7:0] -- exact masks from Linux pcie-brcmstb brcm_pcie_set_outbound_win. */
#define MEM_WIN0_LO             0x400c
#define MEM_WIN0_HI             0x4010
#define MEM_WIN0_BASE_LIMIT     0x4070
#define MEM_WIN0_BASE_HI        0x4080
#define MEM_WIN0_LIMIT_HI       0x4084
/* Extra steps U-Boot brcm_pcie_probe does (values from u-boot bcm2711.h). */
#define MISC_RCB_MPS_MODE       0x400       /* MISC_CTRL: RCB 128B / MPS mode */
#define MSI_INTR2_CLR           0x4508
#define MSI_INTR2_MASK_SET      0x4510
/* v0.4.300+ lead #3: brcmstb LEGACY MSI for BCM2711 (docs/NEXT_20260624_xhci_msi.md). The MSIs
 * live in the CPU INTR2 block (0x4300), bits [31:24]; the 0x4500 block above is the newer-chip
 * MSI block (BCM2711 does not use it). Values from Linux rpi-5.10.y pcie-brcmstb.c. */
#define PCIE_MISC_MSI_BAR_CONFIG_LO  0x4044
#define PCIE_MISC_MSI_BAR_CONFIG_HI  0x4048
#define PCIE_MISC_MSI_DATA_CONFIG    0x404c
#define PCIE_INTR2_CPU_STATUS        0x4300
#define PCIE_INTR2_CPU_CLR           0x4308
#define PCIE_INTR2_CPU_MASK_STATUS   0x430c       /* read-back: 1 = that vector is MASKED */
#define PCIE_INTR2_CPU_MASK_SET      0x4310
#define PCIE_INTR2_CPU_MASK_CLR      0x4314
#define BRCM_MSI_TARGET_ADDR_LO      0xfffffffcU    /* low 32 of the MSI target (same for LT/GT_4GB) */
/* v0.4.301: the MSI target MUST lie OUTSIDE the RC_BAR2 inbound DMA window or the VL805's MSI
 * memory-write is captured as a normal inbound DMA (translated to RAM) instead of reaching the RC
 * MSI controller -> INTR2 never sets. pcie_bringup maps RC_BAR2 = [0, 4GB) identity, so 0xfffffffc
 * (LT_4GB) is INSIDE it. Linux brcm_pcie_setup picks GT_4GB (0xf_fffffffc, just above the window)
 * whenever rc_bar2_offset+size > 0xfffffffc -- which is our case. GT_4GB needs 64-bit MSI on the
 * device (the VL805 is64 cap, confirmed at arm time). Low 32 is unchanged; only the high 32 = 0xf. */
#define BRCM_MSI_TARGET_HI           0xfU           /* high 32 of GT_4GB target 0xf_fffffffc */
#define BRCM_MSI_DATA_CONFIG_VAL8    0xfff86540U    /* legacy 8-MSI data config */
#define BRCM_MSI_DATA_MATCH          0x6540U        /* low 16 of VAL8 = the data the device sends */
#define BRCM_MSI_LEGACY_SHIFT        24             /* MSI vectors in INTR2 bits [31:24] */
#define BRCM_XHCI_MSI_VECTOR         0              /* one vector for the single-function VL805 */
#define BRCM_PCIE_MSI_GIC_SPI        148            /* DTB: GIC_SPI 148 = builtin MSI controller */
#define RC_CFG_VENDOR_SPEC1     0x0188      /* inbound-BAR endian mode (0 = LE) */
#define   VENDOR_ENDIAN_BAR2_MASK 0xc
#define RC_CFG_PRIV1_LINK_CAP   0x04dc
#define   LINK_CAP_ASPM_MASK    0xc00       /* unadvertise ASPM */
/* Internal MDIO bus -> SerDes (for SSC). Offsets + constants from u-boot. */
#define MDIO_PORT0              0
#define MDIO_ADDR_REG           0x1100
#define MDIO_WR_DATA_REG        0x1104
#define MDIO_RD_DATA_REG        0x1108
#define MDIO_DONE               0x80000000u
#define MDIO_DATA_MASK          0x7fffffffu
#define SSC_REGS_ADDR           0x1100
#define SSC_SET_ADDR_OFF        0x1f
#define SSC_CNTL_OFF            0x2
#define SSC_CNTL_OVRD_EN        0x8000
#define SSC_CNTL_OVRD_VAL       0x4000
#define SSC_STATUS_OFF          0x1
#define SSC_STATUS_SSC          0x400       /* bit 10 */
#define SSC_STATUS_PLL_LOCK     0x800       /* bit 11 */
#define BRCM_PCIE_CAP_REGS      0x00ac      /* PCIe capability in RC config */
#define PCI_EXP_LNKCTL          0x10        /* LNKCTL[15:0] | LNKSTA[31:16] dword */

/* VC property mailbox (channel 8) -- used AFTER link-up to load the VL805 fw via
 * NOTIFY_XHCI_RESET. Mailbox + RAM only, so it cannot SError. Reuses the pre-mapped
 * dev_vcmbox_vaddr; the property buffer is pinned LOW (<1GB) at the page after the
 * display's 0x3A000000 so its VC bus alias (| 0xC0000000) is valid. */
#define MBOX_TAG_PCIE_PADDR     0x3A001000UL
#define MBOX_READ_OFF           0x00
#define MBOX_STATUS_OFF         0x18
#define MBOX_WRITE_OFF          0x20
#define MBOX_FULL               0x80000000U
#define MBOX_EMPTY              0x40000000U
#define MBOX_CH_PROP            8
#define MBOX_RESP_OK            0x80000000U
#define RPI_FW_NOTIFY_XHCI_RESET 0x00030058
#define RPI4_VL805_DEVADDR      0x00100000U  /* bus1 dev0 fn0, hardwired PCIe */

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

/* Config write: bus 0 = RC registers direct; bus >= 1 = EXT_CFG index/data window
 * (same addressing as cfg_rd). Used by D.2a to size + place the VL805 BAR. Writes
 * on a live link cannot SError -- same safety as the config reads in D.1. */
static void cfg_wr(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off, uint32_t val) {
    if (bus == 0) { wr(off, val); return; }
    wr(EXT_CFG_INDEX, (bus << 20) | (dev << 15) | (fn << 12));
    arch_dsb();
    *(volatile uint32_t *)(reg + EXT_CFG_DATA + off) = val;
    arch_dsb();
}

/* --- VC property mailbox (for NOTIFY_XHCI_RESET, the VL805 fw load) --- */
static volatile uint32_t *mbox_regs;
static volatile uint32_t *mbox_buf;
static uint64_t mbox_buf_bus;

static int mbox_init(void) {
    if (mbox_buf) return 0;
    if (!dev_vcmbox_vaddr) { AIOS_LOG_WARN("pcie: VC mbox not mapped"); return -1; }
    mbox_regs = (volatile uint32_t *)((uintptr_t)dev_vcmbox_vaddr + dev_vcmbox_off);
    vka_object_t f;
    if (sel4platsupport_alloc_frame_at(&vka, MBOX_TAG_PCIE_PADDR, seL4_PageBits, &f)) {
        AIOS_LOG_WARN("pcie: mbox buf alloc failed"); return -1;
    }
    void *v = vspace_map_pages(&vspace, &f.cptr, NULL, seL4_AllRights, 1, seL4_PageBits, 0);
    if (!v) { AIOS_LOG_WARN("pcie: mbox buf map failed"); return -1; }
    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(f.cptr);
    if (ga.error) { AIOS_LOG_WARN("pcie: mbox buf paddr failed"); return -1; }
    mbox_buf = (volatile uint32_t *)v;
    mbox_buf_bus = ga.paddr | 0xC0000000ULL;
    return 0;
}

static int mbox_send(void) {
    uint32_t addr_ch = (uint32_t)(mbox_buf_bus & 0xFFFFFFF0U) | MBOX_CH_PROP;
    for (uint64_t dl = mono_deadline_ms(2000); mono_before(dl); ) {
        arch_dmb();
        if (!(mbox_regs[MBOX_STATUS_OFF / 4] & MBOX_FULL)) break;
    }
    arch_dsb();
    mbox_regs[MBOX_WRITE_OFF / 4] = addr_ch;
    arch_dsb();
    for (uint64_t dl = mono_deadline_ms(2000); mono_before(dl); ) {
        arch_dmb();
        if (mbox_regs[MBOX_STATUS_OFF / 4] & MBOX_EMPTY) continue;
        arch_dmb();
        if (mbox_regs[MBOX_READ_OFF / 4] == addr_ch) {
            arch_dmb();
            return (mbox_buf[1] == MBOX_RESP_OK) ? 0 : -1;
        }
    }
    return -1;
}

/* One property tag; vals[] in/out, nwords value words. Returns 0 on success. */
static int vc_mbox_tag(uint32_t tag, uint32_t *vals, int nwords) {
    if (mbox_init()) return -1;
    volatile uint32_t *b = mbox_buf;
    b[0] = (uint32_t)((6 + nwords) * 4);
    b[1] = 0;
    b[2] = tag;
    b[3] = (uint32_t)(nwords * 4);
    b[4] = 0;
    for (int k = 0; k < nwords; k++) b[5 + k] = vals[k];
    b[5 + nwords] = 0;
    arch_dsb();
    if (mbox_send()) return -1;
    for (int k = 0; k < nwords; k++) vals[k] = b[5 + k];
    return 0;
}

/* Ask the VC to load the VL805 firmware. Per the RPi contract this must run AFTER
 * the kernel has set up PCI (link up) -- it does NOT power the RC. Without it, on
 * this board the VL805 trains the link but its config space stays 0xffff. */
static void rpi_notify_xhci_reset(void) {
    uint32_t v[1] = { RPI4_VL805_DEVADDR };
    int r = vc_mbox_tag(RPI_FW_NOTIFY_XHCI_RESET, v, 1);
    printf("[pcie] mbox NOTIFY_XHCI_RESET(0x%x): %s (VL805 fw load)\n",
           RPI4_VL805_DEVADDR, r == 0 ? "ok" : "fail/unsupported");
}

/* --- Internal MDIO bus -> SerDes, for Spread Spectrum Clocking (SSC). Ported from
 * u-boot brcm_pcie_mdio_* / brcm_pcie_set_ssc. U-Boot trains the link WITH SSC
 * ("link up ... (SSC)") and we did not -- the one PHY-config step left. --- */
static uint32_t mdio_pkt(uint32_t port, uint32_t regad, uint32_t cmd) {
    return ((port << 16) & 0xf0000u) | (regad & 0xffffu) | ((cmd << 20) & 0xfff00000u);
}

static int mdio_read(uint32_t port, uint32_t regad, uint32_t *val) {
    wr(MDIO_ADDR_REG, mdio_pkt(port, regad, 1 /* READ */));
    (void)rd(MDIO_ADDR_REG);
    for (uint64_t dl = mono_deadline_ms(20); ; ) {
        uint32_t d = rd(MDIO_RD_DATA_REG);
        if (d & MDIO_DONE) { *val = d & MDIO_DATA_MASK; return 0; }
        if (!mono_before(dl)) return -1;
    }
}

static int mdio_write(uint32_t port, uint32_t regad, uint16_t wrdata) {
    wr(MDIO_ADDR_REG, mdio_pkt(port, regad, 0 /* WRITE */));
    (void)rd(MDIO_ADDR_REG);
    wr(MDIO_WR_DATA_REG, MDIO_DONE | wrdata);
    for (uint64_t dl = mono_deadline_ms(20); ; ) {
        if (!(rd(MDIO_WR_DATA_REG) & MDIO_DONE)) return 0;
        if (!mono_before(dl)) return -1;
    }
}

/* Enable Spread Spectrum Clocking on the SerDes. Returns 0 if SSC + PLL lock. */
static int set_ssc(void) {
    uint32_t tmp;
    if (mdio_write(MDIO_PORT0, SSC_SET_ADDR_OFF, SSC_REGS_ADDR)) return -1;
    if (mdio_read(MDIO_PORT0, SSC_CNTL_OFF, &tmp)) return -1;
    tmp |= (SSC_CNTL_OVRD_EN | SSC_CNTL_OVRD_VAL);
    if (mdio_write(MDIO_PORT0, SSC_CNTL_OFF, (uint16_t)tmp)) return -1;
    mdelay(1);
    if (mdio_read(MDIO_PORT0, SSC_STATUS_OFF, &tmp)) return -1;
    int ssc = !!(tmp & SSC_STATUS_SSC);
    int pll = !!(tmp & SSC_STATUS_PLL_LOCK);
    printf("[pcie] SSC: ssc=%d pll_lock=%d\n", ssc, pll);
    return (ssc && pll) ? 0 : -1;
}

/* Read + print the negotiated link speed/width (lnksta in the RC PCIe cap). */
static void print_link_speed(void) {
    uint16_t lnksta = (uint16_t)(rd(BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL) >> 16);
    uint32_t cls = lnksta & 0xf, nlw = (lnksta >> 4) & 0x3f;
    const char *s = cls == 1 ? "2.5" : cls == 2 ? "5.0" : cls == 3 ? "8.0" : "?";
    printf("[pcie] link: %s Gbps x%u (lnksta=0x%04x)\n", s, nlw, lnksta);
}

/* Core bring-up: take the PCIe core out of reset and train the link. ORDER IS
 * LOad-BEARING -- RGR1_SW_INIT_1 (0x9210) is the always-on reset controller, safe
 * to touch while the core is in reset; only AFTER deasserting the bridge reset are
 * the MISC/core regs (0x4xxx: HARD_DEBUG, MISC_CTRL, RC_BAR*, PCIE_STATUS) clocked
 * and safe. Matches U-Boot/Linux/Circle pcie-brcmstb. */
static void bringup(int do_perst) {
    AIOS_LOG_INFO(do_perst ? "brcmstb: FULL bring-up (bridge + PERST reset)"
                           : "brcmstb: gentle bring-up (clock core, preserve EP)");
    /* Always cycle the bridge reset (RGR1, always-on) to clock the RC core. PERST
     * (which RESETS the VL805 and WIPES the firmware the boot firmware loaded into
     * it) is asserted ONLY when do_perst -- the gentle path leaves the VL805 alone
     * and only ensures PERST is released. */
    setb(RGR1_SW_INIT_1, SW_INIT_BRIDGE);   /* assert bridge reset */
    if (do_perst)
        setb(RGR1_SW_INIT_1, SW_INIT_PERST);  /* assert PERST# = reset the VL805 */
    mdelay(2);
    clrb(RGR1_SW_INIT_1, SW_INIT_BRIDGE);   /* deassert bridge reset -> MISC now clocked */
    clrb(HARD_DEBUG, HARD_DEBUG_SERDES_IDDQ);
    mdelay(1);
    /* MISC_CTRL: SCB access + UR mode + RCB/MPS mode + 128B burst (clear mask) +
     * SCB0 size. RCB_MPS_MODE matches U-Boot (we previously omitted it). */
    /* v0.4.213: bound the PCIe transaction timeouts. A transaction the VL805
     * stalls (keyboard-glitch states) sits OUTSTANDING on the UBUS; every DSB
     * on core 0 -- including the kernel's TLBI sequence inside ANY syscall --
     * waits for it, freezing the whole system. The silicon default bounds that
     * wait at ~32.4s (measured); Linux programs ~240/250ms. Print the defaults
     * once (HW evidence), then apply the Linux values. */
    /* HW-measured (v0.4.213): ubus default 0x80000 ticks == the 32.4s system
     * freeze => the 2711 ubus timeout tick is ~16.2kHz (NOT the 2712's 750MHz
     * -- the Linux bcm2712 constants would set HOURS here). 0x1000 ~= 253ms.
     * retry default is 0 on 2711; semantics unknown -- leave it alone. */
    printf("[pcie] timeout defaults: retry=0x%x ubus=0x%x\n",
           rd(RC_CONFIG_RETRY_TIMEOUT), rd(UBUS_TIMEOUT));
    wr(UBUS_TIMEOUT, 0x1000);
    /* v0.4.219: retry default is 0 on 2711 -- possibly "use the (32.4s)
     * silicon default". Bound it like the ubus timeout. */
    wr(RC_CONFIG_RETRY_TIMEOUT, 0x1000);
    printf("[pcie] timeouts set: retry=0x%x ubus=0x%x\n",
           rd(RC_CONFIG_RETRY_TIMEOUT), rd(UBUS_TIMEOUT));

    clrset(MISC_CTRL, MISC_MAX_BURST_MASK | MISC_SCB0_SIZE_MASK,
           MISC_SCB_ACCESS_EN | MISC_CFG_READ_UR | MISC_RCB_MPS_MODE |
           (RC_BAR2_SIZE_3GB_ENC << 27));
    clrb(RC_BAR1_CONFIG_LO, 0x1f);          /* disable GISB window */
    clrb(RC_BAR3_CONFIG_LO, 0x1f);          /* disable SCB window */
    wr(MSI_INTR2_MASK_SET, 0xffffffff);     /* mask all MSI (we handle none yet) */
    wr(MSI_INTR2_CLR, 0xffffffff);          /* clear any pending */
    /* Inbound DMA window RC_BAR2: PCI 0 -> CPU 0 (identity), 4GB. */
    wr(RC_BAR2_CONFIG_LO, 0u | RC_BAR2_SIZE_3GB_ENC);
    wr(RC_BAR2_CONFIG_HI, 0);
    clrb(RGR1_SW_INIT_1, SW_INIT_PERST);    /* ensure PERST released (bit 0) -> link */
    mdelay(100);                            /* PCIe spec post-PERST */
    for (uint64_t dl = mono_deadline_ms(200); mono_before(dl); ) {
        if (link_up()) break;
        mdelay(5);
    }
}

/* Program outbound MEM window 0: CPU 0x6_00000000 -> PCI 0xC0000000, 1GB. brcmstb
 * may not generate downstream TLPs (even config requests) until an outbound window
 * exists -- U-Boot/Linux program it before the bus scan and we previously did not.
 * The window REGISTERS are controller-base-relative (safe to write now); the CPU
 * side >4GB mapping (D.2 / seL4) is only needed to ACCESS the BAR later. Encoding
 * per Linux brcm_pcie_set_outbound_win (base in BASE_LIMIT[15:4], limit in [31:20],
 * high bits >> 12). */
static void set_outbound_win(void) {
    uint64_t cpu = 0x600000000ULL, pci = 0xC0000000ULL, size = 0x40000000ULL;
    uint32_t cpu_mb = (uint32_t)(cpu / 0x100000ULL);
    uint32_t lim_mb = (uint32_t)((cpu + size - 1) / 0x100000ULL);
    wr(MEM_WIN0_LO, (uint32_t)pci);
    wr(MEM_WIN0_HI, (uint32_t)(pci >> 32));
    clrset(MEM_WIN0_BASE_LIMIT, 0xfff0fff0u,
           ((cpu_mb << 4) & 0xfff0u) | ((lim_mb << 20) & 0xfff00000u));
    clrset(MEM_WIN0_BASE_HI,  0xffu, (cpu_mb >> 12) & 0xffu);
    clrset(MEM_WIN0_LIMIT_HI, 0xffu, (lim_mb >> 12) & 0xffu);
    printf("[pcie] outbound win0: CPU 0x%lx -> PCI 0x%lx (1GB)\n",
           (unsigned long)cpu, (unsigned long)pci);
}

/* D.2a: size the VL805 xHCI BAR0, place it at the base of the outbound window
 * (PCI hw_info.pcie_mmio_pci -> CPU hw_info.pcie_mmio_cpu), enable mem-space +
 * bus-master, and record pcie_xhci_* for the shared xHCI driver (src/usb/xhci.c).
 * Mirrors the QEMU pcie_ecam.c BAR sizing (handles a 64-bit BAR). All config-space
 * ops on a live link -- cannot SError. Returns 0 on success. */
static int program_xhci_bar(uint32_t bus, uint32_t dev, uint32_t fn) {
    uint32_t bar0 = cfg_rd(bus, dev, fn, EP_BAR0);
    int is_io = bar0 & 0x1;
    int is64  = !is_io && (((bar0 >> 1) & 0x3) == 0x2);
    if (is_io) { AIOS_LOG_WARN("VL805 BAR0 is I/O space?"); return -1; }

    /* Size: write all-ones, read back, decode (clear low type bits, invert, +1). */
    cfg_wr(bus, dev, fn, EP_BAR0, 0xFFFFFFFF);
    uint32_t szlo = cfg_rd(bus, dev, fn, EP_BAR0);
    uint32_t szhi = 0xFFFFFFFF;
    if (is64) {
        cfg_wr(bus, dev, fn, EP_BAR0 + 4, 0xFFFFFFFF);
        szhi = cfg_rd(bus, dev, fn, EP_BAR0 + 4);
    }
    uint64_t mask = ((uint64_t)szhi << 32) | (uint64_t)(szlo & ~0xFU);
    uint64_t size = ~mask + 1;
    if (size == 0 || size > hw_info.pcie_mmio_size) {
        printf("[pcie] VL805 BAR0 size 0x%lx invalid (window 0x%lx)\n",
               (unsigned long)size, (unsigned long)hw_info.pcie_mmio_size);
        return -1;
    }

    /* Place at the base of the outbound window, naturally aligned. The window CPU
     * base (0x6_00000000) is already 1GB-aligned, so for any xHCI-sized BAR cpu ==
     * the window base and pci == hw_info.pcie_mmio_pci (0xC0000000). */
    uint64_t cpu = (hw_info.pcie_mmio_cpu + (size - 1)) & ~(size - 1);
    uint64_t pci = hw_info.pcie_mmio_pci + (cpu - hw_info.pcie_mmio_cpu);
    cfg_wr(bus, dev, fn, EP_BAR0, (uint32_t)(pci & 0xFFFFFFF0) | (bar0 & 0xF));
    if (is64) cfg_wr(bus, dev, fn, EP_BAR0 + 4, (uint32_t)(pci >> 32));

    /* Enable memory-space decode + bus master. Write 0 to the status half [31:16]
     * (W1C bits ignore a 0 write, so no status is cleared). */
    uint32_t cmd = cfg_rd(bus, dev, fn, EP_COMMAND);
    cfg_wr(bus, dev, fn, EP_COMMAND, (cmd & 0xFFFFu) | PCI_CMD_MEM | PCI_CMD_MASTER);
    arch_dsb();

    pcie_xhci_bar      = cpu;
    pcie_xhci_bar_size = size;
    pcie_xhci_bus = (uint8_t)bus;
    pcie_xhci_dev = (uint8_t)dev;
    pcie_xhci_fn  = (uint8_t)fn;
    printf("[pcie] VL805 BAR0 -> PCI 0x%lx / CPU 0x%lx size 0x%lx%s\n",
           (unsigned long)pci, (unsigned long)cpu, (unsigned long)size,
           is64 ? " (64-bit)" : "");
    return 0;
}

/* Bring up the core + link, program the outbound window, then detect the VL805.
 * bringup() runs the RGR1 reset dance FIRST so the MISC/core regs are clocked
 * before we read them (the v0.4.183 SError was reading them too early). Returns -1
 * (D.1 has no usable xHCI BAR yet -- that is D.2 / gate 2). */
static int pcie_bringup_and_detect(void) {
    reg = (volatile uint8_t *)dev_pcie_vaddr;
    printf("[pcie] brcmstb @ 0x%lx -- core bring-up first (no core read yet)\n",
           (unsigned long)hw_info.pcie_ecam_paddr);

    bringup(1);   /* full reset + PERST + link train (matches U-Boot/Linux) */
    uint32_t st = rd(PCIE_STATUS);
    int up = link_up();
    printf("[pcie] brcmstb rev=0x%04x PCIE_STATUS=0x%x link=%s mode=%s\n",
           rd(MISC_REVISION) & 0xffff, st, up ? "UP" : "DOWN",
           (st & STATUS_PORT) ? "RC" : "EP");
    if (!up) {
        AIOS_LOG_WARN("brcmstb: link DOWN (no device or no link train)");
        return -1;
    }

    /* Tag the RC as a PCIe-PCIe bridge, then program the outbound window BEFORE the
     * first downstream config access (the missing step vs U-Boot). */
    clrset(RC_CFG_PRIV1_ID_VAL3, 0xffffff, 0x060400);
    set_outbound_win();
    /* SSC: the PHY-config step U-Boot does ("(SSC)") that we omitted. Attempt it
     * unconditionally (harmless if unsupported -- the MDIO polls just time out). */
    printf("[pcie] SSC %s\n", set_ssc() == 0 ? "ENABLED" : "not enabled");
    print_link_speed();
#if PCIE_AB_STOP == 1
    printf("[pcie] AB-STOP=1: link trained + outbound window, stopping (no VL805 "
           "fw load / config access / BAR / bus-master)\n");
    return -1;
#endif
    clrb(RC_CFG_VENDOR_SPEC1, VENDOR_ENDIAN_BAR2_MASK);  /* inbound BAR -> little-endian */
    clrb(RC_CFG_PRIV1_LINK_CAP, LINK_CAP_ASPM_MASK);     /* unadvertise ASPM (U-Boot) */
    /* THE missing step vs U-Boot: its generic PCI core sets the RC bridge bus
     * numbers during enumeration so the RC FORWARDS config to bus 1. Without sec=1
     * the brcmstb will not route a config request to bus 1 -> UR -> 0xffff. RC
     * config header is at base+offset (bus 0 = direct). */
    wr(RC_PRIMARY_BUS, (0u) | (1u << 8) | (1u << 16));   /* pri=0 sec=1 sub=1 */
    setb(RC_COMMAND, 0x6);                               /* enable memory + bus master */
    rpi_notify_xhci_reset();   /* harmless; resets the VL805 xHCI if the fw needs it */

    /* Read the VL805 vendor ID (CRS retry ~2s -- live link, config cannot SError). */
    mdelay(100);
    uint32_t id = 0xffffffff;
    for (uint64_t dl = mono_deadline_ms(2000); ; ) {
        id = cfg_rd(1, 0, 0, 0x00);
        if ((id & 0xffff) != 0xffff) break;
        if (!mono_before(dl)) break;
        mdelay(20);
    }
    if ((id & 0xffff) == 0xffff) {
        AIOS_LOG_WARN("brcmstb: bus1 dev0 did not enumerate (0xffff)");
        printf("[pcie] bus1 dev0 = 0xffff -- VL805 not responding (even with outbound win)\n");
        return -1;
    }
    uint16_t vendor = id & 0xffff;
    uint16_t device = (id >> 16) & 0xffff;
    uint32_t cls = cfg_rd(1, 0, 0, 0x08) >> 8;
    printf("[pcie] bus1 dev0: VID=%04x PID=%04x class=%06x\n", vendor, device, cls);

    if (cls != 0x0C0330) {
        printf("[pcie] bus1 dev0 is not an xHCI controller (class 0x%06x)\n", cls);
        return -1;
    }
    printf("[pcie] VL805 xHCI DETECTED -- Phase D.1 link OK.\n");

#if PCIE_AB_STOP == 2
    printf("[pcie] AB-STOP=2: VL805 fw loaded + detected, stopping (no BAR / "
           "mem-space / bus-master -- device cannot DMA)\n");
    return -1;
#endif

    /* D.2a: program the VL805 BAR0 in the outbound window (safe config ops). */
    if (program_xhci_bar(1, 0, 0) != 0) {
        AIOS_LOG_WARN("VL805 BAR0 programming failed");
        return -1;
    }

#if PCIE_AB_STOP == 3
    printf("[pcie] AB-STOP=3: BAR + mem-space + bus-master granted, stopping "
           "(pcie_xhci_present stays 0 -- xHC never reset/started)\n");
    return -1;
#endif

#if PCIE_PROBE_LEVEL >= 2
    /* D.2: hand the BAR to the shared xHCI driver. aios_root sees pcie_xhci_present
     * and calls xhci_init(), which MAPS the BAR (CPU 0x6_00000000, non-cacheable)
     * and drives the controller. The first BAR read is the one inherent SError risk
     * (the outbound window + link are D.1-proven, so it should complete) -- hence
     * this is gated above level 1. The boot-time device-untyped diagnostic in
     * aios_root.c (always on) confirms the BAR is mappable before we get here. */
    pcie_xhci_present = 1;
    printf("[pcie] D.2: pcie_xhci_present=1 -> xhci_init() will map + drive the BAR.\n");
    return 0;
#else
    /* D.2a done (BAR programmed) but xhci_init NOT triggered: the BAR READ is the
     * only remaining SError risk, so it stays opt-in. Flip PCIE_PROBE_LEVEL to 2
     * (or build -DPCIE_PROBE_LEVEL=2) once the device-untyped diagnostic confirms
     * CPU 0x6_00000000 is mappable -- that boot types a key. */
    printf("[pcie] D.2a complete (BAR programmed). xhci_init() GATED at "
           "PCIE_PROBE_LEVEL>=2 (set it to type a key).\n");
    return -1;
#endif
}
#endif /* PCIE_PROBE_LEVEL >= 1 */

int plat_pcie_init(void) {
    if (!hw_info.has_pcie) { AIOS_LOG_WARN("no PCIe in DTB"); return -1; }
    if (!dev_pcie_vaddr) { AIOS_LOG_WARN("PCIe regs not pre-mapped (prealloc)"); return -1; }

#if PCIE_PROBE_LEVEL >= 1
    printf("[pcie] PROBE_LEVEL=%d: brcmstb reset-first bring-up + VL805 detect"
           "%s. If this SErrors -> reflash disk/sdcard-rpi4.img.\n",
           PCIE_PROBE_LEVEL,
           PCIE_PROBE_LEVEL >= 2 ? " + D.2 xhci_init (keyboard)" : " + D.2a BAR program");
    return pcie_bringup_and_detect();
#else
    printf("[pcie] PCIe present (regs 0x%lx) but controller read DISABLED "
           "(PCIE_PROBE_LEVEL=0 -- no SError risk). Build -DPCIE_PROBE_LEVEL=1 to "
           "probe (reset-ordering fix; no EEPROM/mailbox needed).\n",
           (unsigned long)hw_info.pcie_ecam_paddr);
    return -1;
#endif
}

/* Route the VL805 xHCI interrupt to the GIC (lead #3, docs/NEXT_20260624_xhci_msi.md). The VL805
 * raises MSI; the brcmstb root complex has a builtin MSI controller (legacy: CPU INTR2 0x4300,
 * bits[31:24]) that raises GIC_SPI 148. AIOS maps SPI N -> seL4 IRQ N+32, so this returns 180.
 * Binding here is harmless in poll mode (xhci_setup_irq just binds the handler); the RC + VL805
 * MSI are programmed only when the driver enters IRQ mode (plat_pcie_xhci_msi_enable). */
int plat_pcie_xhci_irq(void) {
    if (!pcie_xhci_present) return -1;
    return BRCM_PCIE_MSI_GIC_SPI + 32;   /* GIC_SPI 148 (builtin MSI controller) -> seL4 IRQ 180 */
}

/* Program (on=1) or tear down (on=0) brcmstb legacy MSI for the VL805. Sequence mirrors Linux
 * brcm_msi_set_regs + the device MSI-capability programming. Only called from the driver thread
 * when it enters IRQ mode, so default boot (polling) never touches these registers. */
int plat_pcie_xhci_msi_enable(int on) {
    if (!pcie_xhci_present) return -1;
    uint32_t b = pcie_xhci_bus, d = pcie_xhci_dev, f = pcie_xhci_fn;
    uint32_t legacy_mask = 0xFFu << BRCM_MSI_LEGACY_SHIFT;   /* the 8 MSI bits [31:24] */

    /* Find the device MSI capability (PCI cap ID 0x05) by walking the cap list. */
    uint32_t cap = cfg_rd(b, d, f, 0x34) & 0xFF;
    int msi = -1;
    for (int i = 0; i < 48 && cap >= 0x40; i++) {
        uint32_t h = cfg_rd(b, d, f, cap);
        if ((h & 0xFF) == 0x05) { msi = (int)cap; break; }
        cap = (h >> 8) & 0xFF;
    }
    if (msi < 0) { AIOS_LOG_WARN("xHCI MSI: VL805 MSI capability not found"); return -1; }
    uint32_t mc = cfg_rd(b, d, f, (uint32_t)msi);            /* [31:16] control, [15:0] id|next */
    int is64 = (mc >> (16 + 7)) & 1;                         /* control bit 7 = 64-bit addr cap */
    uint32_t ctrl = (mc >> 16) & 0xFFFF;

    if (!on) {                                               /* disable device cap + re-mask RC */
        cfg_wr(b, d, f, (uint32_t)msi, (mc & 0xFFFF) | ((ctrl & ~0x1u) << 16));
        wr(PCIE_INTR2_CPU_MASK_SET, legacy_mask);
        arch_dsb();
        return 0;
    }

    /* GT_4GB target lives above the 4GB inbound window; it needs 64-bit MSI on the device. If the
     * VL805 is somehow 32-bit-only we cannot reach 0xf_fffffffc -- warn (the probe shows hi will be
     * forced 0, which overlaps the window and won't deliver; the cure would be shrinking RC_BAR2). */
    uint32_t target_hi = is64 ? BRCM_MSI_TARGET_HI : 0u;
    if (!is64)
        AIOS_LOG_WARN("xHCI MSI: VL805 is 32-bit MSI only -- GT_4GB target unreachable (will overlap RC_BAR2)");

    /* RC side: unmask + clear the legacy MSI bits, set the target address + data config. */
    wr(PCIE_INTR2_CPU_MASK_CLR, legacy_mask);
    wr(PCIE_INTR2_CPU_CLR, legacy_mask);
    wr(PCIE_MISC_MSI_BAR_CONFIG_LO, BRCM_MSI_TARGET_ADDR_LO | 1u);
    wr(PCIE_MISC_MSI_BAR_CONFIG_HI, target_hi);            /* 0xf: target ABOVE the 4GB DMA window */
    wr(PCIE_MISC_MSI_DATA_CONFIG, BRCM_MSI_DATA_CONFIG_VAL8);
    arch_dsb();

    /* Device side: Message Address = target, Message Data = match|vector, then enable. */
    cfg_wr(b, d, f, (uint32_t)msi + 0x4, BRCM_MSI_TARGET_ADDR_LO);
    uint32_t data_off = is64 ? ((uint32_t)msi + 0xC) : ((uint32_t)msi + 0x8);
    if (is64) cfg_wr(b, d, f, (uint32_t)msi + 0x8, target_hi);   /* Message Address Hi = 0xf */
    cfg_wr(b, d, f, data_off, BRCM_MSI_DATA_MATCH | BRCM_XHCI_MSI_VECTOR);
    ctrl = (ctrl & ~(0x7u << 4)) | 0x1u;                    /* MME=0 (1 vector), MSI Enable=1 */
    cfg_wr(b, d, f, (uint32_t)msi, (mc & 0xFFFF) | (ctrl << 16));
    arch_dsb();
    printf("[pcie] xHCI MSI armed: cap@0x%x is64=%d target=0x%x%08x data=0x%x -> GIC_SPI %d\n",
           (unsigned)msi, is64, (unsigned)target_hi, (unsigned)BRCM_MSI_TARGET_ADDR_LO,
           (unsigned)(BRCM_MSI_DATA_MATCH | BRCM_XHCI_MSI_VECTOR), BRCM_PCIE_MSI_GIC_SPI);
    return 0;
}

/* Per-interrupt ack: clear our MSI vector's bit in the RC CPU INTR2 (before the seL4 GIC Ack),
 * else the RC keeps the SPI asserted. */
void plat_pcie_xhci_msi_ack(void) {
    if (!pcie_xhci_present) return;
    wr(PCIE_INTR2_CPU_CLR, 1u << (BRCM_MSI_LEGACY_SHIFT + BRCM_XHCI_MSI_VECTOR));
}

/* --- lead #3 debug (docs/NEXT_20260624_xhci_msi.md): split the "MSI armed but count=0" failure ---
 * The RC INTR2 STATUS bit for our vector LATCHES when the VL805's MSI memory-write reaches the RC
 * and matches DATA_CONFIG. While the driver is blocked in seL4_Wait it never acks, so after a
 * keypress this latch tells us, over netconsole, exactly where delivery breaks:
 *   bit SET   -> the VL805 issued the MSI + the RC matched it; nothing reached seL4 IRQ 180, so the
 *                GIC_SPI 148 / IRQ-180 binding is the suspect (try the other SPI).
 *   bit CLEAR -> the VL805 never issued a matching MSI: Message Data, cap-enable, or target addr is
 *                wrong (recheck brcm_msi_compose_msg / the cap walk).
 * Status read is pure + non-destructive (cannot race the driver's ack once MSI works); use
 * plat_pcie_xhci_msi_clear() to wipe the latch between keypresses. */
static volatile uint32_t g_msi_intr2_seen;   /* cumulative: status reads that observed our bit set */

int plat_pcie_xhci_msi_status(char *buf, int bufsize) {
    if (!pcie_xhci_present) return snprintf(buf, bufsize, "msi: xHCI not present\n");
    uint32_t b = pcie_xhci_bus, d = pcie_xhci_dev, f = pcie_xhci_fn;
    unsigned vec_bit = BRCM_MSI_LEGACY_SHIFT + BRCM_XHCI_MSI_VECTOR;   /* INTR2 bit for vector 0 = 24 */
    uint32_t vmask = 1u << vec_bit;

    /* RC side: pure MMIO reads (no config-space window) -- safe from any thread. */
    uint32_t status   = rd(PCIE_INTR2_CPU_STATUS);
    uint32_t maskstat = rd(PCIE_INTR2_CPU_MASK_STATUS);
    uint32_t barlo    = rd(PCIE_MISC_MSI_BAR_CONFIG_LO);
    uint32_t barhi    = rd(PCIE_MISC_MSI_BAR_CONFIG_HI);
    uint32_t dcfg     = rd(PCIE_MISC_MSI_DATA_CONFIG);
    if (status & vmask) g_msi_intr2_seen++;

    int w = snprintf(buf, bufsize,
        "msi-rc: INTR2_STATUS=0x%08x bit%u=%d seen=%u  MASK_STATUS=0x%08x masked%u=%d\n"
        "        BAR_LO=0x%08x BAR_HI=0x%08x DATA_CFG=0x%08x (want LO=0x%08x HI=0x%x DATA=0x%08x)\n",
        status, vec_bit, (status & vmask) ? 1 : 0, g_msi_intr2_seen,
        maskstat, vec_bit, (maskstat & vmask) ? 1 : 0,
        barlo, barhi, dcfg, (unsigned)(BRCM_MSI_TARGET_ADDR_LO | 1u),
        (unsigned)BRCM_MSI_TARGET_HI, BRCM_MSI_DATA_CONFIG_VAL8);

    /* Device side: walk the VL805 cap list for MSI (0x05) and read it back. Config-space access,
     * but at this point only the (idle) enumeration/enable paths share EXT_CFG_INDEX. */
    uint32_t cap = cfg_rd(b, d, f, 0x34) & 0xFF;
    int msi = -1;
    for (int i = 0; i < 48 && cap >= 0x40; i++) {
        uint32_t h = cfg_rd(b, d, f, cap);
        if ((h & 0xFF) == 0x05) { msi = (int)cap; break; }
        cap = (h >> 8) & 0xFF;
    }
    if (msi < 0)
        return w + snprintf(buf + w, bufsize - w, "msi-dev: VL805 MSI capability NOT FOUND\n");

    uint32_t mc   = cfg_rd(b, d, f, (uint32_t)msi);
    uint32_t ctrl = (mc >> 16) & 0xFFFF;
    int is64 = (ctrl >> 7) & 1;
    uint32_t addr_lo = cfg_rd(b, d, f, (uint32_t)msi + 0x4);
    uint32_t addr_hi = is64 ? cfg_rd(b, d, f, (uint32_t)msi + 0x8) : 0;
    uint32_t data    = cfg_rd(b, d, f, is64 ? (uint32_t)msi + 0xC : (uint32_t)msi + 0x8) & 0xFFFF;
    w += snprintf(buf + w, bufsize - w,
        "msi-dev: cap@0x%x ctrl=0x%04x EN=%d MME=%u is64=%d addr=0x%08x%08x data=0x%04x"
        " (want addr=0x%x%08x data=0x%04x)\n",
        (unsigned)msi, ctrl, ctrl & 1, (ctrl >> 4) & 0x7, is64,
        addr_hi, addr_lo, data,
        (unsigned)BRCM_MSI_TARGET_HI, (unsigned)BRCM_MSI_TARGET_ADDR_LO,
        (unsigned)(BRCM_MSI_DATA_MATCH | BRCM_XHCI_MSI_VECTOR));
    return w;
}

/* Wipe the latched RC INTR2 bit (and the seen counter) so a fresh keypress can be observed
 * re-latching it -- the manual equivalent of the driver's ack, for use when the driver is blocked. */
void plat_pcie_xhci_msi_clear(void) {
    if (!pcie_xhci_present) return;
    wr(PCIE_INTR2_CPU_CLR, 1u << (BRCM_MSI_LEGACY_SHIFT + BRCM_XHCI_MSI_VECTOR));
    arch_dsb();
    g_msi_intr2_seen = 0;
}

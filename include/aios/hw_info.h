#ifndef AIOS_HW_INFO_H
#define AIOS_HW_INFO_H

#include <stdint.h>

/* Hardware inventory populated from DTB at boot.
 * Replaces hardcoded MMIO addresses with discovered values. */

typedef struct {
    /* UART */
    uint64_t uart_paddr;        /* PL011 base (QEMU: 0x9000000) */
    uint32_t uart_irq;          /* SPI number (QEMU: 33) */
    int      has_uart;

    /* Virtio MMIO region */
    uint64_t virtio_base;       /* first virtio MMIO (QEMU: 0xa000000) */
    uint32_t virtio_size;       /* region size per device */
    int      virtio_count;      /* number of virtio devices found in DTB */
    int      has_virtio;

    /* fw_cfg (QEMU firmware config) */
    uint64_t fwcfg_paddr;       /* QEMU: 0x09020000 */
    int      has_fwcfg;

    /* eMMC/SDHCI (RPi) */
    uint64_t emmc_paddr;        /* BCM2835 SDHCI base */
    uint32_t emmc_irq;
    int      has_emmc;

    /* GENET Ethernet (RPi4) */
    uint64_t genet_paddr;       /* BCM54213 GENET base */
    uint32_t genet_irq;
    int      has_genet;

    /* VideoCore mailbox (RPi) */
    uint64_t vc_mbox_paddr;     /* ARM-to-VC mailbox base */
    int      has_vc_mbox;

    /* V3D 4.2 GPU (RPi4) -- hub+core MMIO base (ARM phys); IRQ = GIC SPI 74 + 32 */
    uint64_t v3d_paddr;         /* 0xFEC00000 (hub at +0, core0 at +0x4000) */
    uint32_t v3d_irq;           /* seL4 IRQ 106 */
    int      has_v3d;

    /* PCIe host bridge (QEMU virt: pci-host-ecam-generic; RPi4: brcmstb) */
    uint64_t pcie_ecam_paddr;   /* ECAM config-space base */
    uint64_t pcie_ecam_size;
    uint64_t pcie_mmio_cpu;     /* CPU-side base of the 32-bit MMIO window (map BARs here) */
    uint64_t pcie_mmio_pci;     /* PCI-side base of that window (value written into BARs) */
    uint64_t pcie_mmio_size;
    uint32_t pcie_bus_start;
    uint32_t pcie_bus_end;
    int      has_pcie;

    /* CPU */
    int      cpu_count;
    char     cpu_compat[32];    /* e.g. "arm,cortex-a53" */

    /* Memory */
    uint64_t mem_base;
    uint64_t mem_size;

    /* DTB state */
    int      dtb_valid;         /* 1 if DTB parsed successfully */
} aios_hw_info_t;

/* Global instance -- populated by boot_dtb_init() */
extern aios_hw_info_t hw_info;

/* Parse DTB from seL4 bootinfo extras and populate hw_info.
 * Falls back to QEMU virt defaults if DTB is absent or malformed. */
void boot_dtb_init(void);

/* Print discovered hardware inventory to serial */
void boot_hw_report(void);

/* Live SoC readouts via the VC firmware mailbox (RPi4; impl src/gpu/v3d.c, which
 * owns the HW-verified low-pinned tag buffer). Fill *cur (+ *max if non-NULL) and
 * return 0 on success, or -1 if unavailable (QEMU has no mailbox). cur/max are in
 * Hz for the ARM clock and millidegrees C for the SoC temperature. Surfaced via
 * /proc/cpufreq and /proc/temp. */
int hw_arm_clock_hz(unsigned int *cur_hz, unsigned int *max_hz);
int hw_soc_temp_mc(int *cur_mc, int *max_mc);

#endif /* AIOS_HW_INFO_H */

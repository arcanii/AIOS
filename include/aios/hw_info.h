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

#endif /* AIOS_HW_INFO_H */

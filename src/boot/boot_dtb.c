/*
 * boot_dtb.c -- Device Tree Blob parser for hardware discovery
 *
 * Extracts UART, virtio MMIO, fw_cfg, CPU, and memory info from
 * the DTB provided by seL4 via bootinfo extras.  Falls back to
 * QEMU virt defaults if DTB is unavailable or unparseable.
 */
#include <stdio.h>
#include <string.h>
#include <sel4/sel4.h>
#include <simple/simple.h>
#include <libfdt.h>
#include "aios/hw_info.h"

/* QEMU virt platform defaults (fallback if no DTB) */
#define DEFAULT_UART_PADDR    0x9000000UL
#define DEFAULT_UART_IRQ      33
#define DEFAULT_VIRTIO_BASE   0xa000000UL
#define DEFAULT_VIRTIO_SIZE   0x200
#define DEFAULT_FWCFG_PADDR   0x09020000UL

aios_hw_info_t hw_info;

extern simple_t simple;

static void set_defaults(void) {
    memset(&hw_info, 0, sizeof(hw_info));
    hw_info.uart_paddr   = DEFAULT_UART_PADDR;
    hw_info.uart_irq     = DEFAULT_UART_IRQ;
    hw_info.has_uart     = 1;
    hw_info.virtio_base  = DEFAULT_VIRTIO_BASE;
    hw_info.virtio_size  = DEFAULT_VIRTIO_SIZE;
    hw_info.virtio_count = 0;
    hw_info.has_virtio   = 1;
    hw_info.fwcfg_paddr  = DEFAULT_FWCFG_PADDR;
    hw_info.has_fwcfg    = 0;
    hw_info.cpu_count    = 1;
    hw_info.mem_base     = 0;
    hw_info.mem_size     = 0;
    hw_info.dtb_valid    = 0;
    strncpy(hw_info.cpu_compat, "unknown", sizeof(hw_info.cpu_compat) - 1);
}

/* Read a 64-bit value from two consecutive 32-bit BE cells.
 * fdt32_ld() is provided by libfdt. */
static uint64_t fdt_read_reg64(const void *p) {
    const fdt32_t *c = (const fdt32_t *)p;
    return ((uint64_t)fdt32_ld(&c[0]) << 32) | fdt32_ld(&c[1]);
}

static void parse_uart(const void *fdt) {
    int node = fdt_node_offset_by_compatible(fdt, -1, "arm,pl011");
    if (node < 0) return;

    int len;
    const void *reg = fdt_getprop(fdt, node, "reg", &len);
    if (reg && len >= 8) {
        hw_info.uart_paddr = fdt_read_reg64(reg);
        hw_info.has_uart = 1;
    }

    const void *irq = fdt_getprop(fdt, node, "interrupts", &len);
    if (irq && len >= 12) {
        /* GIC format: <type irq_num flags>
         * type 0 = SPI, irq_num is SPI offset, actual = irq_num + 32 */
        const fdt32_t *ic = (const fdt32_t *)irq;
        uint32_t irq_type = fdt32_ld(&ic[0]);
        uint32_t irq_num  = fdt32_ld(&ic[1]);
        if (irq_type == 0)
            hw_info.uart_irq = irq_num + 32;
        else
            hw_info.uart_irq = irq_num;
    }
}

static void parse_virtio(const void *fdt) {
    int count = 0;
    uint64_t lowest_addr = (uint64_t)-1;
    int node = -1;

    while (1) {
        node = fdt_node_offset_by_compatible(fdt, node, "virtio,mmio");
        if (node < 0) break;
        count++;

        int len;
        const void *reg = fdt_getprop(fdt, node, "reg", &len);
        if (reg && len >= 8) {
            uint64_t addr = fdt_read_reg64(reg);
            if (addr < lowest_addr)
                lowest_addr = addr;
            if (len >= 16) {
                const fdt32_t *vc = (const fdt32_t *)reg;
                hw_info.virtio_size = (uint32_t)fdt_read_reg64(&vc[2]);
            }
        }
    }

    hw_info.virtio_count = count;
    if (count > 0 && lowest_addr != (uint64_t)-1) {
        hw_info.virtio_base = lowest_addr;
        hw_info.has_virtio = 1;
    }
}

static void parse_fwcfg(const void *fdt) {
    int node = fdt_node_offset_by_compatible(fdt, -1, "qemu,fw-cfg-mmio");
    if (node < 0) return;

    int len;
    const void *reg = fdt_getprop(fdt, node, "reg", &len);
    if (reg && len >= 8) {
        hw_info.fwcfg_paddr = fdt_read_reg64(reg);
        hw_info.has_fwcfg = 1;
    }
}

static void parse_cpus(const void *fdt) {
    int cpus = fdt_path_offset(fdt, "/cpus");
    if (cpus < 0) return;

    int count = 0;
    int child;
    fdt_for_each_subnode(child, fdt, cpus) {
        int dtl;
        const void *dt = fdt_getprop(fdt, child, "device_type", &dtl);
        if (dt && dtl >= 4 && memcmp(dt, "cpu", 3) == 0) {
            count++;
            if (count == 1) {
                int len;
                const void *compat = fdt_getprop(fdt, child, "compatible", &len);
                if (compat && len > 0) {
                    int cl = len < (int)sizeof(hw_info.cpu_compat) - 1 ?
                             len : (int)sizeof(hw_info.cpu_compat) - 1;
                    memcpy(hw_info.cpu_compat, compat, cl);
                    hw_info.cpu_compat[cl] = 0;
                }
            }
        }
    }
    if (count > 0) hw_info.cpu_count = count;
}

static void parse_memory(const void *fdt) {
    int node = fdt_path_offset(fdt, "/memory");
    if (node < 0) {
        /* Try /memory@... naming convention */
        node = fdt_node_offset_by_prop_value(fdt, -1, "device_type",
                                              "memory", 7);
    }
    if (node < 0) return;

    int len;
    const void *reg = fdt_getprop(fdt, node, "reg", &len);
    if (reg && len >= 16) {
        const fdt32_t *mc = (const fdt32_t *)reg;
        hw_info.mem_base = fdt_read_reg64(&mc[0]);
        hw_info.mem_size = fdt_read_reg64(&mc[2]);
    }
}

void boot_dtb_init(void) {
    set_defaults();

    /* Get DTB from seL4 bootinfo extras */
    ssize_t dtb_len = simple_get_extended_bootinfo_length(
        &simple, SEL4_BOOTINFO_HEADER_FDT);
    if (dtb_len <= 0) {
        printf("[dtb] No DTB in bootinfo (using defaults)\n");
        return;
    }

    /* Allocate buffer on stack (DTBs are typically 8-64KB) */
    if (dtb_len > 65536) {
        printf("[dtb] DTB too large: %ld bytes\n", (long)dtb_len);
        return;
    }

    char dtb_buf[65536];
    ssize_t copied = simple_get_extended_bootinfo(
        &simple, SEL4_BOOTINFO_HEADER_FDT,
        dtb_buf, (unsigned long)dtb_len);
    if (copied != dtb_len) {
        printf("[dtb] DTB copy failed (%ld/%ld)\n", (long)copied, (long)dtb_len);
        return;
    }

    /* Skip seL4 bootinfo header (id + len = 2 words = 16 bytes on 64-bit) */
    const void *fdt = dtb_buf + 2 * sizeof(seL4_Word);
    int fdt_size = (int)(dtb_len - 2 * (int)sizeof(seL4_Word));

    if (fdt_check_header(fdt) != 0) {
        printf("[dtb] Invalid FDT header\n");
        return;
    }

    hw_info.dtb_valid = 1;

    parse_uart(fdt);
    parse_virtio(fdt);
    parse_fwcfg(fdt);
    parse_cpus(fdt);
    parse_memory(fdt);
}

void boot_hw_report(void) {
    printf("[hw] DTB: %s\n", hw_info.dtb_valid ? "parsed" : "defaults");
    printf("[hw] CPU: %d core(s), %s\n",
           hw_info.cpu_count, hw_info.cpu_compat);
    if (hw_info.mem_size)
        printf("[hw] RAM: %lu MB @ 0x%lx\n",
               (unsigned long)(hw_info.mem_size / (1024 * 1024)),
               (unsigned long)hw_info.mem_base);
    printf("[hw] UART: 0x%lx IRQ %u\n",
           (unsigned long)hw_info.uart_paddr, hw_info.uart_irq);
    if (hw_info.has_virtio)
        printf("[hw] Virtio: %d device(s) @ 0x%lx\n",
               hw_info.virtio_count, (unsigned long)hw_info.virtio_base);
    if (hw_info.has_fwcfg)
        printf("[hw] fw_cfg: 0x%lx\n", (unsigned long)hw_info.fwcfg_paddr);
}

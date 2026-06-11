/*
 * boot_dtb.c -- Device Tree Blob parser for hardware discovery
 *
 * Extracts UART, virtio MMIO, fw_cfg, CPU, and memory info from
 * the DTB provided by seL4 via bootinfo extras.  Falls back to
 * QEMU virt defaults if DTB is unavailable or unparseable.
 */
#define LOG_MODULE "hw"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "aios/aios_log.h"
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

/* Read address from DTB reg property, respecting parent address-cells.
 * RPi4 soc nodes use #address-cells=1 (32-bit), emmc2bus uses 2 (64-bit).
 * fdt and node must be valid; fdt_address_cells / fdt_parent_offset from libfdt. */
static uint64_t fdt_read_reg_node(const void *fdt, int node) {
    int len;
    const void *reg = fdt_getprop(fdt, node, "reg", &len);
    if (!reg || len < 4) return 0;

    const fdt32_t *c = (const fdt32_t *)reg;

    /* Check parent #address-cells to determine read width */
    int parent = fdt_parent_offset(fdt, node);
    int addr_cells = 2;  /* default 64-bit */
    if (parent >= 0) {
        int ac = fdt_address_cells(fdt, parent);
        if (ac > 0) addr_cells = ac;
    }

    uint64_t addr;
    if (addr_cells == 1) {
        addr = (uint64_t)fdt32_ld(&c[0]);
    } else if (len >= 8) {
        addr = ((uint64_t)fdt32_ld(&c[0]) << 32) | fdt32_ld(&c[1]);
    } else {
        addr = (uint64_t)fdt32_ld(&c[0]);
    }

    /* BCM2711: translate VC bus address to ARM physical.
     * VC peripherals at 0x7Exxxxxx map to ARM 0xFExxxxxx (+0x80000000).
     * QEMU addresses pass through unchanged. */
    if (addr >= 0x7C000000UL && addr <= 0x7FFFFFFFUL)
        addr += 0x80000000UL;

    return addr;
}

/* Legacy wrapper for code that passes raw reg pointer */
static uint64_t fdt_read_reg64(const void *p) {
    const fdt32_t *c = (const fdt32_t *)p;
    return ((uint64_t)fdt32_ld(&c[0]) << 32) | fdt32_ld(&c[1]);
}

static void parse_uart(const void *fdt) {
    int node = fdt_node_offset_by_compatible(fdt, -1, "arm,pl011");
    if (node < 0) return;

    hw_info.uart_paddr = fdt_read_reg_node(fdt, node);
    if (hw_info.uart_paddr) hw_info.has_uart = 1;

    int len;
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

static void parse_emmc(const void *fdt) {
    /* BCM2711 eMMC2 or BCM2835 SDHCI */
    int node = fdt_node_offset_by_compatible(fdt, -1, "brcm,bcm2711-emmc2");
    if (node < 0)
        node = fdt_node_offset_by_compatible(fdt, -1, "brcm,bcm2835-sdhci");
    if (node < 0) return;

    hw_info.emmc_paddr = fdt_read_reg_node(fdt, node);
    if (hw_info.emmc_paddr) hw_info.has_emmc = 1;

    int len;
    const void *irq = fdt_getprop(fdt, node, "interrupts", &len);
    if (irq && len >= 12) {
        const fdt32_t *ic = (const fdt32_t *)irq;
        uint32_t irq_type = fdt32_ld(&ic[0]);
        uint32_t irq_num  = fdt32_ld(&ic[1]);
        hw_info.emmc_irq = (irq_type == 0) ? irq_num + 32 : irq_num;
    }
}

static void parse_genet(const void *fdt) {
    int node = fdt_node_offset_by_compatible(fdt, -1, "brcm,bcm2711-genet-v5");
    if (node < 0) return;

    hw_info.genet_paddr = fdt_read_reg_node(fdt, node);
    if (hw_info.genet_paddr) hw_info.has_genet = 1;

    int len;
    const void *irq = fdt_getprop(fdt, node, "interrupts", &len);
    if (irq && len >= 12) {
        const fdt32_t *ic = (const fdt32_t *)irq;
        uint32_t irq_type = fdt32_ld(&ic[0]);
        uint32_t irq_num  = fdt32_ld(&ic[1]);
        hw_info.genet_irq = (irq_type == 0) ? irq_num + 32 : irq_num;
    }
}

static void parse_vc_mbox(const void *fdt) {
    int node = fdt_node_offset_by_compatible(fdt, -1, "brcm,bcm2835-mbox");
    if (node < 0) return;

    hw_info.vc_mbox_paddr = fdt_read_reg_node(fdt, node);
    if (hw_info.vc_mbox_paddr) hw_info.has_vc_mbox = 1;
}

static void parse_pcie(const void *fdt) {
    /* QEMU virt: "pci-host-ecam-generic"; RPi4 (Phase D): "brcm,bcm2711-pcie". */
    int node = fdt_node_offset_by_compatible(fdt, -1, "pci-host-ecam-generic");
    if (node < 0)
        node = fdt_node_offset_by_compatible(fdt, -1, "brcm,bcm2711-pcie");
    printf("[hw] pcie DTB node: %s\n", node < 0 ? "ABSENT" : "present");

    if (node >= 0) {
        int len;
        /* reg = controller/ECAM base + size (base 2 cells + size 2 cells). */
        const void *reg = fdt_getprop(fdt, node, "reg", &len);
        if (reg && len >= 16) {
            const fdt32_t *c = (const fdt32_t *)reg;
            uint64_t base = ((uint64_t)fdt32_ld(&c[0]) << 32) | fdt32_ld(&c[1]);
            /* BCM2711: VC bus 0x7Dxx/0x7Exx -> ARM 0xFDxx/0xFExx (+0x80000000). */
            if (base >= 0x7C000000UL && base <= 0x7FFFFFFFUL) base += 0x80000000UL;
            hw_info.pcie_ecam_paddr = base;
            hw_info.pcie_ecam_size  = ((uint64_t)fdt32_ld(&c[2]) << 32) | fdt32_ld(&c[3]);
        }
        const void *br = fdt_getprop(fdt, node, "bus-range", &len);
        if (br && len >= 8) {
            const fdt32_t *b = (const fdt32_t *)br;
            hw_info.pcie_bus_start = fdt32_ld(&b[0]);
            hw_info.pcie_bus_end   = fdt32_ld(&b[1]);
        }
        /* ranges: pci_addr(3) cpu_addr(2) size(2). space bits[25:24]: 2=MMIO32. */
        const void *rg = fdt_getprop(fdt, node, "ranges", &len);
        if (rg) {
            const fdt32_t *r = (const fdt32_t *)rg;
            int cells = len / 4;
            for (int i = 0; i + 7 <= cells; i += 7) {
                uint32_t space = (fdt32_ld(&r[i]) >> 24) & 0x3;
                if (space == 0x2) {
                    hw_info.pcie_mmio_pci  = ((uint64_t)fdt32_ld(&r[i + 1]) << 32) | fdt32_ld(&r[i + 2]);
                    hw_info.pcie_mmio_cpu  = ((uint64_t)fdt32_ld(&r[i + 3]) << 32) | fdt32_ld(&r[i + 4]);
                    hw_info.pcie_mmio_size = ((uint64_t)fdt32_ld(&r[i + 5]) << 32) | fdt32_ld(&r[i + 6]);
                }
            }
        }
        if (hw_info.pcie_ecam_paddr && hw_info.pcie_mmio_size)
            hw_info.has_pcie = 1;
    }

#ifdef PLAT_RPI4
    /* RPi4/BCM2711 fixed-address fallback (USB HID Phase D). The firmware-modified
     * runtime DTB does NOT expose a usable brcmstb PCIe MMIO window, so the parse
     * above leaves has_pcie=0. Force the known BCM2711 PCIe addresses so
     * plat_pcie_init runs. The earlier crash (2026-06-08, "halting... Kernel entry
     * via Unknown") was an UNGUARDED first read of a POWER-GATED controller; that
     * guard now exists -- pcie_brcmstb.c confirms power via the VC mailbox before
     * any MMIO and gates the controller read behind PCIE_PROBE_LEVEL (default
     * diagnostic-only, no read). So setting has_pcie here is now SAFE. See
     * docs/DESIGN_USB_HID.md "HW RESULT". QEMU is unaffected (compiled out). */
    if (!hw_info.has_pcie) {
        hw_info.pcie_ecam_paddr = 0xFD500000UL;   /* brcmstb controller registers */
        hw_info.pcie_ecam_size  = 0x0000A000UL;   /* 10 pages (covers up to 0x9310) */
        hw_info.pcie_mmio_pci   = 0xC0000000UL;    /* PCI side of the outbound window */
        hw_info.pcie_mmio_cpu   = 0x600000000UL;   /* CPU side (needs D.2 kernel window) */
        hw_info.pcie_mmio_size  = 0x40000000UL;    /* 1 GB outbound window */
        hw_info.pcie_bus_start  = 0;
        hw_info.pcie_bus_end    = 1;
        hw_info.has_pcie        = 1;
        printf("[hw] pcie: BCM2711 fixed-address fallback (regs 0x%lx) -- Phase D\n",
               (unsigned long)hw_info.pcie_ecam_paddr);
    }
#endif
}

static void parse_v3d(const void *fdt) {
    /* V3D 4.2 GPU. Cloned from parse_genet: compatible brcm,2711-v3d, VC-bus
     * +0x80000000 fixup (in fdt_read_reg_node), IRQ = SPI num + 32. */
    int node = fdt_node_offset_by_compatible(fdt, -1, "brcm,2711-v3d");
    if (node >= 0) {
        uint64_t base = fdt_read_reg_node(fdt, node);   /* first reg = hub base */
        if (base) { hw_info.v3d_paddr = base; hw_info.has_v3d = 1; }

        int len;
        const void *irq = fdt_getprop(fdt, node, "interrupts", &len);
        if (irq && len >= 12) {
            const fdt32_t *ic = (const fdt32_t *)irq;
            uint32_t irq_type = fdt32_ld(&ic[0]);
            uint32_t irq_num  = fdt32_ld(&ic[1]);
            hw_info.v3d_irq = (irq_type == 0) ? irq_num + 32 : irq_num;
        }
    }

#ifdef PLAT_RPI4
    /* RPi4/BCM2711 fixed-address fallback (like parse_pcie). The firmware runtime
     * DTB strips the gpu node, and downstream trees nest v3d under a v3dbus wrapper
     * whose path is not stable, so the parse above almost always leaves has_v3d=0 on
     * real hardware. Force the known V3D base + IRQ. Reads of the gated V3D block are
     * HW-safe and v3d_init touches NO V3D MMIO (binds IRQ 106 masked, power deferred
     * to an explicit /proc/v3d.power poke), so asserting this here is safe. */
    if (!hw_info.has_v3d) {
        hw_info.v3d_paddr = 0xFEC00000UL;   /* hub @ +0, core0 @ +0x4000 */
        hw_info.has_v3d   = 1;
        printf("[hw] v3d: BCM2711 fixed-address fallback (0x%lx)\n",
               (unsigned long)hw_info.v3d_paddr);
    }
    if (!hw_info.v3d_irq) hw_info.v3d_irq = 106;   /* GIC SPI 74 + 32 */
#endif
}

static void parse_memory(const void *fdt) {
    int node = fdt_path_offset(fdt, "/memory");
    if (node < 0) {
        /* Try /memory@... naming convention */
        node = fdt_node_offset_by_prop_value(fdt, -1, "device_type",
                                              "memory", 7);
    }
    if (node < 0) return;

    int parent = fdt_parent_offset(fdt, node);
    int ac = 2, sc = 1;
    if (parent >= 0) {
        int a = fdt_address_cells(fdt, parent);
        int s = fdt_size_cells(fdt, parent);
        if (a > 0) ac = a;
        if (s > 0) sc = s;
    }

    int len;
    const void *reg = fdt_getprop(fdt, node, "reg", &len);
    if (!reg) return;

    const fdt32_t *mc = (const fdt32_t *)reg;
    int stride = ac + sc;  /* cells per memory region entry */
    int entries = (len / 4) / stride;

    /* Sum all memory region sizes */
    uint64_t total = 0;
    for (int i = 0; i < entries; i++) {
        const fdt32_t *e = &mc[i * stride];
        uint64_t base = (ac == 2)
            ? ((uint64_t)fdt32_ld(&e[0]) << 32) | fdt32_ld(&e[1])
            : (uint64_t)fdt32_ld(&e[0]);
        uint64_t size = (sc == 2)
            ? ((uint64_t)fdt32_ld(&e[ac]) << 32) | fdt32_ld(&e[ac + 1])
            : (uint64_t)fdt32_ld(&e[ac]);
        if (i == 0) hw_info.mem_base = base;
        total += size;
    }
    hw_info.mem_size = total;
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

    /* Static buffer (BSS): the user-task root stack is 16 KB
     * (CONFIG_SEL4RUNTIME_ROOT_STACK), so a 64 KB stack array would
     * overflow downward into BSS and silently corrupt nearby globals.
     * v0.4.116 chased a layout-sensitive bug that traced back to this. */
    static char dtb_buf[65536];
    if (dtb_len > (ssize_t)sizeof(dtb_buf)) {
        printf("[dtb] DTB too large: %ld bytes\n", (long)dtb_len);
        return;
    }
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
    parse_emmc(fdt);
    parse_genet(fdt);
    parse_vc_mbox(fdt);
    parse_pcie(fdt);
    parse_v3d(fdt);
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
    if (hw_info.has_emmc)
        printf("[hw] eMMC: 0x%lx IRQ %u\n",
               (unsigned long)hw_info.emmc_paddr, hw_info.emmc_irq);
    if (hw_info.has_genet)
        printf("[hw] GENET: 0x%lx IRQ %u\n",
               (unsigned long)hw_info.genet_paddr, hw_info.genet_irq);
    if (hw_info.has_vc_mbox)
        printf("[hw] VC mbox: 0x%lx\n",
               (unsigned long)hw_info.vc_mbox_paddr);
    if (hw_info.has_v3d)
        printf("[hw] V3D: 0x%lx IRQ %u\n",
               (unsigned long)hw_info.v3d_paddr, hw_info.v3d_irq);
    if (hw_info.has_pcie)
        printf("[hw] PCIe: ECAM 0x%lx, MMIO32 0x%lx (size 0x%lx), bus %u-%u\n",
               (unsigned long)hw_info.pcie_ecam_paddr,
               (unsigned long)hw_info.pcie_mmio_cpu,
               (unsigned long)hw_info.pcie_mmio_size,
               hw_info.pcie_bus_start, hw_info.pcie_bus_end);
}

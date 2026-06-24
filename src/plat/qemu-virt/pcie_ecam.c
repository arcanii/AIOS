/*
 * pcie_ecam.c -- generic PCIe ECAM root complex (QEMU virt)
 *
 * USB HID arc Layer 1 (QEMU side). See docs/DESIGN_USB_HID.md. Enumerates bus 0
 * via the ECAM config space parsed from the DTB (pci-host-ecam-generic), finds
 * the xHCI controller, assigns + enables its BAR0 from the 32-bit MMIO window,
 * and records it for the shared xHCI driver (Phase B).
 *
 * QEMU boots with -kernel (no firmware), so PCI BARs are UNASSIGNED -- we must
 * size and place them ourselves. The RPi4 brcmstb backend (Phase D) provides the
 * same plat_pcie_init() with the BCM2711 config-access + link-training sequence.
 */
#include "aios/root_shared.h"
#include "aios/vka_audit.h"
#include <sel4platsupport/device.h>
#include <stdio.h>
#include "arch.h"
#include "aios/hw_info.h"
#include "aios/pcie.h"
#define LOG_MODULE "pcie"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "aios/aios_log.h"

/* Discovered xHCI controller (consumed by Phase B). */
uint64_t pcie_xhci_bar = 0;
uint64_t pcie_xhci_bar_size = 0;
uint8_t  pcie_xhci_bus = 0, pcie_xhci_dev = 0, pcie_xhci_fn = 0;
int      pcie_xhci_present = 0;

/* PCI config-space offsets */
#define PCI_VENDOR_ID    0x00
#define PCI_COMMAND      0x04
#define PCI_CLASS_REV    0x08   /* [31:8] = class/subclass/progif, [7:0] = revision */
#define PCI_BAR0         0x10
#define PCI_CMD_MEM      0x0002 /* memory-space decode enable */
#define PCI_CMD_MASTER   0x0004 /* bus-master enable */
#define XHCI_CLASS       0x0C0330 /* serial-bus / USB / xHCI programming interface */

/* Map the 4 KB ECAM config region for (bus,dev,fn) into the root vspace as
 * device memory (non-cacheable). Returns the root vaddr or NULL; the frame is
 * returned via *frame for cfg_unmap(). */
static volatile uint8_t *cfg_map(uint8_t bus, uint8_t dev, uint8_t fn,
                                 vka_object_t *frame) {
    uint64_t pa = hw_info.pcie_ecam_paddr +
                  ((uint64_t)bus << 20) + ((uint64_t)dev << 15) + ((uint64_t)fn << 12);
    if (sel4platsupport_alloc_frame_at(&vka, pa, seL4_PageBits, frame))
        return NULL;
    void *va = vspace_map_pages(&vspace, &frame->cptr, NULL,
                                seL4_AllRights, 1, seL4_PageBits, 0 /* non-cacheable */);
    if (!va) { vka_free_object(&vka, frame); return NULL; }
    return (volatile uint8_t *)va;
}

static void cfg_unmap(volatile uint8_t *va, vka_object_t *frame) {
    if (va) vspace_unmap_pages(&vspace, (void *)va, 1, seL4_PageBits, NULL);
    vka_free_object(&vka, frame);
}

static inline uint32_t cfg_r32(volatile uint8_t *c, int off) {
    return *(volatile uint32_t *)(c + off);
}
static inline void cfg_w32(volatile uint8_t *c, int off, uint32_t v) {
    *(volatile uint32_t *)(c + off) = v;
}
static inline uint16_t cfg_r16(volatile uint8_t *c, int off) {
    return *(volatile uint16_t *)(c + off);
}
static inline void cfg_w16(volatile uint8_t *c, int off, uint16_t v) {
    *(volatile uint16_t *)(c + off) = v;
}

int plat_pcie_init(void) {
    if (!hw_info.has_pcie) {
        AIOS_LOG_WARN("no PCIe host bridge in DTB");
        return -1;
    }
    printf("[pcie] ECAM 0x%lx, MMIO window 0x%lx size 0x%lx\n",
           (unsigned long)hw_info.pcie_ecam_paddr,
           (unsigned long)hw_info.pcie_mmio_cpu,
           (unsigned long)hw_info.pcie_mmio_size);

    /* Bump allocator for BAR placement, tracked in CPU address space. */
    uint64_t mmio_next = hw_info.pcie_mmio_cpu;
    uint64_t mmio_end  = hw_info.pcie_mmio_cpu + hw_info.pcie_mmio_size;
    int bus = (int)hw_info.pcie_bus_start;

    for (int dev = 0; dev < 32; dev++) {
        vka_object_t fr;
        volatile uint8_t *c = cfg_map((uint8_t)bus, (uint8_t)dev, 0, &fr);
        if (!c) continue;

        uint32_t id = cfg_r32(c, PCI_VENDOR_ID);
        uint16_t vendor = (uint16_t)(id & 0xFFFF);
        if (vendor == 0xFFFF) { cfg_unmap(c, &fr); continue; }  /* empty slot */
        uint16_t device = (uint16_t)((id >> 16) & 0xFFFF);
        uint32_t classcode = cfg_r32(c, PCI_CLASS_REV) >> 8;    /* 24-bit class code */

        if (classcode != XHCI_CLASS) { cfg_unmap(c, &fr); continue; }

        /* Found xHCI. Size BAR0 (memory BAR, possibly 64-bit). */
        uint32_t bar0 = cfg_r32(c, PCI_BAR0);
        int is_io = bar0 & 0x1;
        int is64  = !is_io && (((bar0 >> 1) & 0x3) == 0x2);
        if (is_io) {                       /* xHCI is always MMIO; bail if not */
            AIOS_LOG_WARN("xHCI BAR0 is I/O space?");
            cfg_unmap(c, &fr);
            return -1;
        }
        cfg_w32(c, PCI_BAR0, 0xFFFFFFFF);
        uint32_t szlo = cfg_r32(c, PCI_BAR0);
        uint32_t szhi = 0xFFFFFFFF;
        if (is64) {
            cfg_w32(c, PCI_BAR0 + 4, 0xFFFFFFFF);
            szhi = cfg_r32(c, PCI_BAR0 + 4);
        }
        /* Decode size: clear the low type bits, invert, +1. */
        uint64_t mask = ((uint64_t)szhi << 32) | (uint64_t)(szlo & ~0xFU);
        uint64_t size = ~mask + 1;
        if (size == 0) { cfg_unmap(c, &fr); return -1; }

        /* Place it in the 32-bit MMIO window, naturally aligned. */
        uint64_t base = (mmio_next + (size - 1)) & ~(size - 1);
        if (base + size > mmio_end) {
            AIOS_LOG_WARN("xHCI BAR does not fit in MMIO window");
            cfg_unmap(c, &fr);
            return -1;
        }
        /* The BAR holds a PCI address; CPU access is at the CPU address. On virt
         * the two windows are identity, but compute the PCI value explicitly. */
        uint64_t bar_pci = hw_info.pcie_mmio_pci + (base - hw_info.pcie_mmio_cpu);
        cfg_w32(c, PCI_BAR0, (uint32_t)(bar_pci & 0xFFFFFFF0) | (bar0 & 0xF));
        if (is64) cfg_w32(c, PCI_BAR0 + 4, (uint32_t)(bar_pci >> 32));

        /* Enable memory decode + bus master. */
        uint16_t cmd = cfg_r16(c, PCI_COMMAND);
        cfg_w16(c, PCI_COMMAND, cmd | PCI_CMD_MEM | PCI_CMD_MASTER);
        arch_dsb();

        mmio_next = base + size;
        pcie_xhci_bar = base;
        pcie_xhci_bar_size = size;
        pcie_xhci_bus = (uint8_t)bus;
        pcie_xhci_dev = (uint8_t)dev;
        pcie_xhci_fn  = 0;
        pcie_xhci_present = 1;

        printf("[pcie] xHCI %02x:%02x.0 vendor=%04x device=%04x BAR0=0x%lx size=0x%lx%s\n",
               bus, dev, vendor, device,
               (unsigned long)base, (unsigned long)size, is64 ? " (64-bit)" : "");
        AIOS_LOG_INFO_V("xHCI BAR0=0x", (unsigned long)base);

        cfg_unmap(c, &fr);
        return 0;
    }

    AIOS_LOG_WARN("no xHCI controller on PCIe bus");
    return -1;
}

/* PCI config: Interrupt Pin (1=INTA..4=INTD) and the Command "Interrupt Disable" bit. */
#define PCI_INT_PIN      0x3D
#define PCI_CMD_INTX_DIS (1u << 10)
/* QEMU virt routes the four PCIe INTx lines to GIC SPIs starting at SPI 3
 * (irqmap[VIRT_PCIE] in hw/arm/virt.c), swizzled per device: GSI = (slot + pin) % 4 with
 * pin 0-indexed (INTA=0). The full GIC IRQ is 32 + 3 + GSI. (See docs/NEXT note: confirm
 * against the virt PCIe node interrupt-map if a controller lands on a different slot.) */
#define VIRT_PCIE_SPI_BASE 3

int plat_pcie_xhci_irq(void) {
    if (!pcie_xhci_present) return -1;
    vka_object_t fr;
    volatile uint8_t *c = cfg_map(pcie_xhci_bus, pcie_xhci_dev, pcie_xhci_fn, &fr);
    if (!c) return -1;
    uint8_t pin = c[PCI_INT_PIN];                  /* 1=INTA .. 4=INTD, 0=none */
    /* AIOS uses neither MSI nor MSI-X, so qemu-xhci asserts legacy INTx; make sure the
     * Command register Interrupt Disable bit is clear so the line is delivered. */
    uint16_t cmd = cfg_r16(c, PCI_COMMAND);
    cfg_w16(c, PCI_COMMAND, cmd & ~PCI_CMD_INTX_DIS);
    arch_dsb();
    cfg_unmap(c, &fr);
    if (pin == 0 || pin > 4) { AIOS_LOG_WARN("xHCI has no INTx pin"); return -1; }
    int gsi = ((int)pcie_xhci_dev + (int)(pin - 1)) % 4;
    int irq = 32 + VIRT_PCIE_SPI_BASE + gsi;
    printf("[pcie] xHCI INTx pin=%u dev=%u -> GIC IRQ %d (SPI %d)\n",
           pin, pcie_xhci_dev, irq, VIRT_PCIE_SPI_BASE + gsi);
    return irq;
}

/* QEMU xHCI uses legacy INTx through the gpex bridge (enabled in plat_pcie_xhci_irq above), not
 * brcmstb MSI -- so MSI program/ack are no-ops here. (arm-virt has no xHCI in practice anyway.) */
int  plat_pcie_xhci_msi_enable(int on) { (void)on; return 0; }
void plat_pcie_xhci_msi_ack(void) { }

#ifndef AIOS_PCIE_H
#define AIOS_PCIE_H

#include <stdint.h>

/*
 * PCIe Layer 1 (USB HID arc -- see docs/DESIGN_USB_HID.md).
 *
 * plat_pcie_init() is the per-platform PCIe root-complex bring-up: it enumerates
 * the bus, finds the xHCI controller (class 0x0C0330), assigns + enables its
 * BAR0, and records it below for the shared xHCI driver (Phase B, src/usb/).
 *   - QEMU virt:  src/plat/qemu-virt/pcie_ecam.c  (generic ECAM)
 *   - RPi4:       src/plat/rpi4/pcie_brcmstb.c    (brcmstb, Phase D)
 *
 * Returns 0 if an xHCI controller was found + programmed, negative otherwise.
 */
int plat_pcie_init(void);

/* Route the xHCI controller's interrupt to the GIC and return the seL4 IRQ number
 * (the full GIC number, i.e. 32 + SPI), or -1 if IRQ delivery is not available (the
 * driver then stays in polling mode). Called once after plat_pcie_init() succeeds.
 *   - QEMU virt: the xHCI INTx line through the gpex host bridge to a GIC SPI.
 *   - RPi4 brcmstb: the VL805 MSI into the root-complex MSI controller (HW-pending).
 * Used by the xHCI driver for IRQ-driven operation (Task 2, opt-in via /proc/xhci.irq). */
int plat_pcie_xhci_irq(void);

/* v0.4.300+ lead #3 (docs/NEXT_20260624_xhci_msi.md): program / tear down MSI delivery for the
 * xHCI controller. on=1 wires the brcmstb RC MSI controller + the device MSI capability so the
 * VL805 raises MSI -> GIC_SPI 148; on=0 reverts. Returns 0 on success, -1 if unavailable. Only
 * called when the driver enters IRQ mode (/proc/xhci.irq.1) -- default boot stays in polling.
 * plat_pcie_xhci_msi_ack() clears the RC MSI INTR2 bit once per interrupt (before the GIC Ack).
 * Both are no-ops on QEMU (its xHCI uses INTx through the gpex bridge, not brcmstb MSI). */
int  plat_pcie_xhci_msi_enable(int on);
void plat_pcie_xhci_msi_ack(void);

/* Discovered xHCI controller (valid after plat_pcie_init() returns 0). */
extern uint64_t pcie_xhci_bar;   /* CPU-side MMIO base of the xHCI BAR0 */
extern uint64_t pcie_xhci_bar_size;
extern uint8_t  pcie_xhci_bus;
extern uint8_t  pcie_xhci_dev;
extern uint8_t  pcie_xhci_fn;
extern int      pcie_xhci_present;

#endif /* AIOS_PCIE_H */

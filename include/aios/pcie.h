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

/* Discovered xHCI controller (valid after plat_pcie_init() returns 0). */
extern uint64_t pcie_xhci_bar;   /* CPU-side MMIO base of the xHCI BAR0 */
extern uint64_t pcie_xhci_bar_size;
extern uint8_t  pcie_xhci_bus;
extern uint8_t  pcie_xhci_dev;
extern uint8_t  pcie_xhci_fn;
extern int      pcie_xhci_present;

#endif /* AIOS_PCIE_H */

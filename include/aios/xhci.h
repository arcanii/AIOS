#ifndef AIOS_XHCI_H
#define AIOS_XHCI_H

#include <stdint.h>

/*
 * xHCI host controller -- shared, platform-independent (USB HID arc Layer 2,
 * docs/DESIGN_USB_HID.md). Driven by whichever PCIe backend (Phase A on QEMU,
 * Phase D on RPi4) found + programmed the controller's BAR (pcie_xhci_bar).
 *
 * xhci_init() maps the register space, resets the controller, allocates the
 * DCBAA / command ring / event ring, runs it, and reports the connected ports.
 * Returns 0 if the controller reached the running state, negative otherwise.
 */
int xhci_init(void);

#endif /* AIOS_XHCI_H */

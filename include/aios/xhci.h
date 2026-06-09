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

/* Reserve the controller DMA pool. MUST be called EARLY in boot (before fs/display
 * consume the low RAM) so the rings land below the RPi4 3GB inbound DMA window;
 * xhci_init() calls it again as an idempotent fallback. Harmless where there is no
 * DMA-window limit (e.g. QEMU). */
int xhci_dma_reserve(void);

/* Live diagnostic exposed at /proc/xhci (wired in src/procfs.c). args is the path
 * suffix after "xhci" (e.g. ".led.7", ".lock", or ""). Writes a human-readable
 * snapshot / command result into buf; returns the byte count. Lets the controller +
 * keyboard be inspected and the lock LEDs poked from the shell without reflashing. */
int xhci_diag_cmd(const char *args, char *buf, int bufsize);

/* /proc/mouse -- the system mouse state (x, y, buttons, event count) accumulated from
 * USB mouse reports. Writes a human-readable line into buf; returns the byte count. */
int xhci_mouse_state(char *buf, int bufsize);

#endif /* AIOS_XHCI_H */

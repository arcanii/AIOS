/*
 * pcie_brcmstb.c -- BCM2711 PCIe root complex (RPi4) -- STUB
 *
 * USB HID arc Layer 1 (RPi4 side), Phase D. The BCM2711 has a single-lane PCIe
 * Gen 2.0 root complex at 0xFD500000 with the VIA VL805 xHCI controller as the
 * only device. Bringing it up needs the brcmstb reset + PERST# + link-training
 * sequence (see docs/DESIGN_USB_HID.md "Phase D"); until then this is a stub so
 * the platform-agnostic boot path links. Develop Layers 2-5 on QEMU first.
 */
#include <stdio.h>
#include "aios/hw_info.h"
#include "aios/pcie.h"

uint64_t pcie_xhci_bar = 0;
uint64_t pcie_xhci_bar_size = 0;
uint8_t  pcie_xhci_bus = 0, pcie_xhci_dev = 0, pcie_xhci_fn = 0;
int      pcie_xhci_present = 0;

int plat_pcie_init(void) {
    /* Phase D: brcmstb PCIe link bring-up + VL805 enumeration not implemented. */
    printf("[pcie] RPi4 brcmstb PCIe not yet implemented (USB HID Phase D)\n");
    return -1;
}

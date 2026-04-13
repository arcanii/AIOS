# AIOS USB HID Keyboard Design

## Executive Summary

RPi4 has 4 USB-A ports, all behind a **VIA VL805** USB 3.0 controller connected
via **PCIe Gen 2.0 x1** to the BCM2711 SoC. Getting USB keyboard input requires
five driver layers: PCIe root complex, xHCI host controller, USB device
enumeration, HID class driver, and keymap translation.

This is a multi-version effort (estimated 4000-6000 lines, 11-17 weeks).

## RPi4 USB Topology

```
USB-A Port 1 --|
USB-A Port 2 --|-- VIA VL805 (xHCI) --[PCIe x1]--> BCM2711
USB-A Port 3 --|
USB-A Port 4 --|

USB-C Port --------- BCM2711 DWC2 OTG (USB 2.0, shared with power)
```

All four USB-A ports require the full PCIe + xHCI stack. The USB-C port uses a
separate DWC2 controller (simpler but shared with power supply).

## Driver Layers

### Layer 1: BCM2711 PCIe Root Complex

**Files:** `src/plat/rpi4/pcie_bcm2711.c` (~1000 lines)

The BCM2711 has a single-lane PCIe Gen 2.0 root complex at 0xFD500000 (ARM
physical). The VL805 is the only device on the bus.

Required operations:
- ECAM configuration space access (Type 0/1 config reads/writes)
- Bridge window setup (memory BAR, bus number assignment)
- VL805 BAR0 discovery (xHCI register base)
- MSI or legacy interrupt routing via GIC
- Reset and link training sequence

DTB compatible: `brcm,bcm2711-pcie`

Reference: Linux `drivers/pci/controller/pcie-brcmstb.c`

### Layer 2: xHCI Host Controller (VL805)

**Files:** `src/plat/rpi4/usb_xhci.c` (~2500 lines)

The VL805 presents a standard xHCI 1.0 interface. xHCI is complex but
well-documented (USB.org specification, 600+ pages).

Required data structures:
- Device Context Base Address Array (DCBAA)
- Command Ring (TRBs for host-initiated commands)
- Event Ring (TRBs for completion/status notifications)
- Transfer Rings (one per endpoint, TRBs for data transfer)
- Scratchpad buffers (controller-specified count)

Required operations:
- Controller reset and initialization
- Port status change detection (device connect/disconnect)
- Slot enable + address device
- Configure endpoint (interrupt IN for HID)
- Transfer ring management (enqueue TRBs, process completions)

IRQ: MSI from VL805 through PCIe, or legacy INTx via GIC

Reference: Linux `drivers/usb/host/xhci.c`, `xhci-ring.c`, `xhci-mem.c`

### Layer 3: USB Device Enumeration

**Files:** `src/plat/rpi4/usb_enum.c` (~800 lines)

Standard USB enumeration sequence via control transfers:

1. Reset port, detect speed (FS/HS/SS)
2. GET_DESCRIPTOR (device descriptor, 18 bytes)
3. SET_ADDRESS (assign unique device address)
4. GET_DESCRIPTOR (configuration descriptor, full tree)
5. Parse interface descriptors, find HID class (bInterfaceClass=3)
6. SET_CONFIGURATION (activate the configuration)
7. Parse endpoint descriptors (interrupt IN endpoint)

### Layer 4: USB HID Keyboard Driver

**Files:** `src/plat/rpi4/hid_keyboard.c` (~400 lines)

HID boot keyboard protocol is simple (8-byte reports):

```
Byte 0: Modifier keys (Shift, Ctrl, Alt, GUI)
Byte 1: Reserved
Byte 2-7: Up to 6 simultaneous key scancodes
```

Required operations:
- SET_PROTOCOL (boot protocol, simpler than report protocol)
- SET_IDLE (suppress unchanged reports)
- Schedule periodic interrupt IN transfers
- Parse 8-byte boot reports
- Detect key press/release (compare with previous report)

### Layer 5: Keymap + Integration

**Files:** `src/plat/rpi4/usb_keymap.c` (~200 lines)

- USB HID scancode to ASCII translation table (104 keys)
- Modifier key handling (Shift, Ctrl, Alt)
- Key repeat timer (optional, can defer)
- Feed keystrokes to root task main loop

Integration point: same as UART input path in `aios_root.c`:
```c
seL4_SetMR(0, (seL4_Word)ascii_char);
seL4_Call(serial_ep.cptr, seL4_MessageInfo_new(SER_KEY_PUSH, 0, 0, 1));
```

## HAL Extension

### New header: `src/plat/input_hal.h`

```c
/* Input device HAL -- keyboard input for RPi4 USB */
int  plat_input_init(void);        /* init USB/PCIe stack */
int  plat_input_poll(char *c);     /* non-blocking: 1 if key, 0 if none */
void plat_input_driver_fn(void *); /* driver thread (IRQ-driven) */
```

QEMU virt does not need this (UART serves as keyboard input).

### hw_info.h additions

```c
uint64_t pcie_paddr;       /* BCM2711 PCIe root complex base */
uint32_t pcie_irq;         /* PCIe interrupt */
int      has_pcie;
```

### DTB parser addition

```c
static void parse_pcie(const void *fdt) {
    int node = fdt_node_offset_by_compatible(fdt, -1, "brcm,bcm2711-pcie");
    ...
}
```

## DWC2 Alternative

The BCM2711 has a DWC2 (DesignWare USB 2.0) controller at 0xFE980000, connected
to the USB-C port. This is simpler than PCIe + xHCI:

- No PCIe layer needed (direct MMIO)
- Simpler host controller protocol than xHCI
- Estimate: ~1500 lines total

However, the USB-C port is shared with power supply. Using it for keyboard
input would require a USB-C hub or OTG adapter, which is impractical for
normal use. This option exists but is not recommended.

## Phased Implementation

### Phase U1 (v0.4.93+): PCIe Root Complex
- Map PCIe ECAM from DTB
- Link training, bus enumeration
- Discover VL805 (vendor 0x1106, device 0x3483)
- Assign BARs
- **Test:** "VL805 found at bus 0" on HDMI console

### Phase U2 (v0.4.94+): xHCI Initialization
- Map VL805 BAR0 (xHCI registers)
- Allocate DCBAA, command ring, event ring
- Controller reset + run
- Detect port status changes
- **Test:** "xHCI operational, N ports" on HDMI console

### Phase U3 (v0.4.95+): USB Enumeration
- Port reset + speed detection
- Device descriptor, SET_ADDRESS, SET_CONFIGURATION
- Find HID keyboard interface
- **Test:** "USB keyboard detected: VID:PID" on HDMI console

### Phase U4 (v0.4.96+): HID Keyboard
- SET_PROTOCOL (boot), SET_IDLE
- Interrupt IN transfers (polling or IRQ)
- Scancode-to-ASCII, modifier keys
- Feed to tty_server via SER_KEY_PUSH
- **Test:** type on keyboard, see characters on HDMI

## Effort Estimates

| Layer | Lines | Weeks | Complexity |
|-------|-------|-------|------------|
| PCIe root complex | 800-1200 | 3-4 | Very high |
| xHCI controller | 2000-3000 | 4-6 | Very high |
| USB enumeration | 600-1000 | 2-3 | High |
| HID keyboard | 300-500 | 1-2 | Medium |
| Keymap + integration | 200-400 | 1-2 | Low |
| **Total** | **4000-6000** | **11-17** | |

## References

- USB xHCI 1.2 specification (usb.org)
- Linux `drivers/pci/controller/pcie-brcmstb.c`
- Linux `drivers/usb/host/xhci.c`
- Linux `drivers/hid/usbhid/hid-core.c`
- Circle bare-metal USB: `lib/usb/` (simpler reference)
- BCM2711 ARM Peripherals datasheet (section 5: PCIe)

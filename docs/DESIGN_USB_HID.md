# AIOS USB HID Keyboard Design

## Executive Summary

RPi4 has 4 USB-A ports, all behind a **VIA VL805** USB 3.0 controller connected
via **PCIe Gen 2.0 x1** to the BCM2711 SoC. Getting USB keyboard input requires
five driver layers: PCIe root complex, xHCI host controller, USB device
enumeration, HID class driver, and keymap translation.

This is a multi-version effort (estimated 4000-6000 lines). The bulk is built and
tested on QEMU (see Development Strategy below); only the PCIe root complex is
hardware-specific, which keeps the effort tractable despite the line count.

## Development Strategy (QEMU-first)

**Do NOT develop this blind on hardware.** `qemu-system-aarch64 -machine virt`
exposes a generic PCIe ECAM host bridge, and a spec-compliant xHCI controller
plus a USB HID keyboard are two flags:

    -device qemu-xhci,id=xhci -device usb-kbd

Keystrokes come from the QEMU display window (or `sendkey` on the QEMU monitor).
So the parts where you iterate the most (xHCI ring management, enumeration) get
second-scale turnaround on QEMU instead of a flash per change. Only **Layer 1**
is target-specific:

| Layer | QEMU virt | RPi4 |
|-------|-----------|------|
| 1. PCIe root complex | `pci-host-ecam-generic` (standard ECAM, simple) | `brcm,bcm2711-pcie` (link training + BCM2711 quirks, HW-only) |
| 2. xHCI controller | identical -- qemu-xhci is spec xHCI 1.0 | identical -- VL805 is spec xHCI 1.0 |
| 3. USB enumeration | identical | identical |
| 4. HID boot keyboard | identical | identical |
| 5. keymap + integration | identical | identical |

**File organization follows this split:** Layers 2-5 are platform-independent and
live in a shared `src/usb/`; Layer 1 has one backend per platform under
`src/plat/{qemu-virt,rpi4}/`, selected behind `input_hal.h`. Build Layers
1(generic-ECAM)+2+3+4+5 on QEMU until you can type into the AIOS shell, then write
ONLY the brcmstb PCIe backend for the Pi and reuse Layers 2-5 unchanged. Because
qemu-xhci and the VL805 are both spec xHCI, once brcmstb PCIe attaches the VL805
the controller/enumeration/HID code should largely just work.

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

### Layer 1: PCIe Root Complex (per-platform)

**Files:** `src/plat/qemu-virt/pcie_ecam.c` (~400 lines, generic ECAM -- build
this first) and `src/plat/rpi4/pcie_brcmstb.c` (~1000 lines, HW), both behind
`src/plat/input_hal.h`.

On QEMU virt this is the standard `pci-host-ecam-generic` bridge (plain ECAM
config access, no link training). On RPi4 the BCM2711 has a single-lane PCIe Gen
2.0 root complex at 0xFD500000 (ARM physical) with the VL805 as the only device
on the bus -- it needs the brcmstb reset + link-training sequence.

Required operations:
- ECAM configuration space access (Type 0/1 config reads/writes)
- Bridge window setup (memory BAR, bus number assignment)
- VL805 BAR0 discovery (xHCI register base)
- MSI or legacy interrupt routing via GIC
- Reset and link training sequence

DTB compatible: `brcm,bcm2711-pcie`

Reference: Linux `drivers/pci/controller/pcie-brcmstb.c`

### Layer 2: xHCI Host Controller (shared)

**Files:** `src/usb/xhci.c` (~2500 lines) -- platform-independent; the same code
drives qemu-xhci and the VL805.

Both qemu-xhci and the VL805 present a standard xHCI 1.0 interface. xHCI is
complex but well-documented (USB.org specification, 600+ pages).

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

### Layer 3: USB Device Enumeration (shared)

**Files:** `src/usb/usb_enum.c` (~800 lines) -- platform-independent.

Standard USB enumeration sequence via control transfers:

1. Reset port, detect speed (FS/HS/SS)
2. GET_DESCRIPTOR (device descriptor, 18 bytes)
3. SET_ADDRESS (assign unique device address)
4. GET_DESCRIPTOR (configuration descriptor, full tree)
5. Parse interface descriptors, find HID class (bInterfaceClass=3)
6. SET_CONFIGURATION (activate the configuration)
7. Parse endpoint descriptors (interrupt IN endpoint)

### Layer 4: USB HID Keyboard Driver (shared)

**Files:** `src/usb/hid_keyboard.c` (~400 lines) -- platform-independent.

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

### Layer 5: Keymap + Integration (shared)

**Files:** `src/usb/usb_keymap.c` (~200 lines) -- platform-independent.

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
int  plat_input_init(void);        /* attach the platform PCIe backend + init xHCI */
int  plat_input_poll(char *c);     /* non-blocking: 1 if key, 0 if none */
void plat_input_driver_fn(void *); /* driver thread (IRQ-driven) */
```

Both platforms implement this: `plat_input_init` wires the platform PCIe backend
(generic ECAM on QEMU, brcmstb on RPi4) to the shared xHCI driver in `src/usb/`.
On QEMU, UART still serves as the normal console -- the xHCI path is what you
exercise while developing the stack (boot with `-device qemu-xhci -device
usb-kbd`), and it is the same code that drives the keyboard on real hardware.

### hw_info.h additions

```c
uint64_t pcie_paddr;       /* BCM2711 PCIe root complex base */
uint32_t pcie_irq;         /* PCIe interrupt */
int      has_pcie;
```

### DTB parser addition

```c
static void parse_pcie(const void *fdt) {
    /* QEMU virt: "pci-host-ecam-generic"; RPi4: "brcm,bcm2711-pcie".
     * Pull the ECAM base from "reg" and the bus range / MMIO windows from
     * "bus-range" and "ranges". */
    int node = fdt_node_offset_by_compatible(fdt, -1, "pci-host-ecam-generic");
    if (node < 0)
        node = fdt_node_offset_by_compatible(fdt, -1, "brcm,bcm2711-pcie");
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

## Phased Implementation (QEMU-first)

Phases A-C are developed + verified entirely on QEMU (fast iteration, no flash).
Phase D is the only hardware-only part: the brcmstb PCIe backend. Boot QEMU with
`-device qemu-xhci,id=xhci -device usb-kbd` for A-C.

### Phase A -- Generic PCIe ECAM (QEMU)
- Parse the `pci-host-ecam-generic` node from the QEMU virt DTB (ECAM base,
  bus-range, MMIO/IO ranges).
- ECAM config-space reads/writes; enumerate bus 0.
- Find the qemu-xhci device (class 0x0C0330); read + program BAR0 (xHCI MMIO).
- **Test:** AIOS_LOG "xHCI found at <bdf>, BAR0=<addr>".

### Phase B -- xHCI initialization (QEMU)
- Map BAR0; parse capability / operational / runtime / doorbell registers.
- Allocate DCBAA, command ring, event ring, scratchpad buffers.
- Controller reset (HCRST), set CONFIG.MaxSlotsEn, run (R/S=1).
- Enable interrupter 0 (poll the event ring first; IRQ comes later).
- Detect port status changes (PORTSC) when usb-kbd attaches.
- **Test:** "xHCI operational, N ports, device on port P".

### Phase C -- Enumeration + HID + keymap (QEMU)
- Enable slot, Address Device (EP0 control transfer ring).
- GET_DESCRIPTOR (device + config); find the HID boot-keyboard interface
  (class 3, subclass 1, protocol 1); SET_CONFIGURATION.
- SET_PROTOCOL(boot), SET_IDLE; configure the interrupt-IN endpoint.
- Schedule periodic interrupt-IN transfers; parse 8-byte boot reports (modifier
  byte + up to 6 scancodes); diff vs the previous report for press/release.
- Scancode->ASCII keymap (+ Shift/Ctrl); feed each char to tty_server via
  SER_KEY_PUSH -- the SAME path UART input uses today.
- **Test:** type in the QEMU window -> characters appear in the AIOS shell.

### Phase D -- RPi4 brcmstb PCIe (HARDWARE)
- New backend `src/plat/rpi4/pcie_brcmstb.c` behind input_hal.h: PCIe at
  0xFD500000, reset + PERST# + wait for link-up, set inbound/outbound windows,
  assign bus/BAR for the VL805 (vendor 0x1106, device 0x3483).
- IRQ via GIC (legacy INTx first; MSI later).
- Reuse Layers 2-5 (`src/usb/`) unchanged.
- **Test (HW):** plug a USB keyboard into a USB-A port, type, see it on HDMI.
- Gotcha: check whether the RPi firmware already powered + trained the VL805
  (it may need a firmware blob load, or the bootloader may have left it ready).

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

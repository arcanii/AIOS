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

## Phase D findings (brcmstb bring-up + the seL4 device-window BLOCKER)

Research done 2026-06-07 (U-Boot/Linux pcie-brcmstb + the RPi4 DTB + the seL4
bcm2711 device map). Two things to know before writing the driver:

### BLOCKER: the PCIe MMIO window is outside seL4's physical address space
The RPi4 DTB routes the PCIe outbound window at **CPU 0x6_00000000 -> PCI
0xC0000000, 1 GB** (`ranges = <0x2000000 0 0xc0000000 0x06 0 0 0x40000000>`). The
xHCI BAR lives in that window, so AIOS must map CPU ~0x6_00000000 to touch the
controller. BUT seL4's bcm2711 kernel (`build-rpi4/kernel/gen_headers/plat/
machine/devices_gen.h`) only knows RAM `[0x200000,0x3a000000)` + `[0x40000000,
0xfc000000)` and a few device frames (UART 0xfe215000, GIC 0xff84_xxxx, ARM-local
0xff800000) -- **nothing above 0x1_00000000**. So `sel4platsupport_alloc_frame_at`
cannot allocate a frame at 0x6_00000000: the window is outside seL4's universe.
The controller REGISTERS (0xFD500000, in `[0xfc000000,0x100000000)`) ARE mappable,
and config access goes through them (EXT_CFG), so the link bring-up + VL805
config detect work WITHOUT the window. Only the xHCI BAR (MMIO) is blocked.
Relocating the window into `[0xfc000000,0x100000000)` is NOT viable -- that range
is real BCM2711 peripherals (GENET 0xfd58, eMMC 0xfe34, ...).
**RESOLUTION: extend the seL4 bcm2711 kernel to expose the PCIe window above 4 GB**
-- add the region to the kernel hardware spec / `deps/kernel/src/plat/bcm2711/
overlay-rpi4.dts` so it lands in devices_gen.h as user-available device memory,
and ensure the kernel's max paddr covers it. Kernel change (gitignored deps/,
re-apply on reset), kernel rebuild, HW-iterated. THEN AIOS can claim the window
(prealloc_rpi4_devices, ascending order) + the xHCI BAR.

### Controller-register claim ordering
The controller regs (0xFD500000, ~10 pages) sit BELOW GENET (0xFD580000), so they
must be claimed in `prealloc_rpi4_devices` BEFORE GENET (the ascending-paddr
watermark, [[feedback_sel4_device_untyped_order]]). Add `dev_pcie_vaddr` there.

### Bring-up sequence (U-Boot pcie_brcmstb, exact offsets from base 0xFD500000)
RGR1_SW_INIT_1=0x9210 (PERST=bit0: 0=assert/1=deassert; bridge INIT_GENERIC=bit1:
1=assert/0=deassert). HARD_DEBUG=0x4204 (SERDES_IDDQ=BIT(27)). MISC_CTRL=0x4008
(SCB_ACCESS_EN=0x1000, CFG_READ_UR_MODE=0x2000, MAX_BURST_MASK=0x300000 [128=0 on
2711], SCB0_SIZE_MASK=0xf8000000). PCIE_STATUS=0x4068 (PORT=0x80, DL_ACTIVE=0x20,
PHYLINKUP=0x10). Outbound win0: MEM_WIN0_LO=0x400c, _HI=0x4010, BASE_LIMIT=0x4070
(base[31:20]+limit[15:4], MB units), BASE_HI=0x4080, LIMIT_HI=0x4084 (>>12).
Inbound RC_BAR2_CONFIG_LO=0x4034/_HI=0x4038 (size enc = log2(roundup)-15; 3GB->17),
RC_BAR1_LO=0x402c + RC_BAR3_LO=0x403c disable (clear low 5 bits). EXT_CFG_INDEX=
0x9000, EXT_CFG_DATA=0x8000 (cfg: bus0 = base+off direct; bus>=1 = write
(bus<<20|dev<<15|fn<<12) to INDEX then base+0x8000+off). RC class code reg
PRIV1_ID_VAL3=0x043c (set 0x060400), endian VENDOR_SPECIFIC_REG1=0x0188, ASPM
PRIV1_LINK_CAPABILITY=0x04dc. Sequence: bridge-reset+PERST assert -> 1ms ->
bridge-reset deassert -> SERDES_IDDQ off -> ~0.2ms -> MISC_CTRL + inbound window +
disable BAR1/3 + mask MSI -> PERST deassert -> 100ms -> poll PCIE_STATUS until
DL_ACTIVE&PHYLINKUP (100ms) -> outbound window -> RC class/endian/ASPM -> scan
bus 1 for the VL805 xHCI (class 0x0C0330), size+assign BAR0 in the PCI window,
enable. pcie_xhci_bar (CPU side for xhci.c) = 0x6_00000000 + (bar_pci-0xC0000000).
PERST polarity + the MB window encoding are the most error-prone -- log every
step. DMA inbound is identity (PCI==CPU via RC_BAR2), so xhci.c GetAddress paddrs
work directly as the controller's DMA addresses.

### HW RESULT (2026-06-08) + the VL805/PCIe power-on mechanism
Flashed + serial-tested on a real RPi4. The pcie node IS in AIOS's DTB
("[hw] pcie DTB node: present") but the firmware-modified runtime `ranges` does
not parse into a usable MMIO window, so has_pcie stayed false (plat_pcie_init
skipped). Forcing has_pcie via the fixed BCM2711 addresses so plat_pcie_init ran
**CRASHED the boot**: "[boot] Display: 1024x768" -> "halting... Kernel entry via
Unknown (0)", with NO [pcie] printf -- the FIRST controller MMIO read
(rd(MISC_REVISION/PCIE_STATUS)) SError'd. The PCIe controller is POWERED DOWN at
OS handoff. (Reverted the fallback -> safe boot.)

**vl805.bin is NOT the fix and is NOT needed.** The VL805 firmware lives in the
bootloader SPI EEPROM (merged into the main boot EEPROM on Pi4B rev 1.4+);
vl805.bin on SD is only an EEPROM-update blob (and 404 in the firmware repo).
Per the rpi-eeprom docs, the VL805 firmware is "loaded ON REQUEST FROM THE KERNEL
if VL805=1 in the EEPROM config" -- the OS must ask the firmware to bring up the
VL805/PCIe. So Phase D needs, BEFORE any controller MMIO access, ONE of:
1. **EEPROM config (no AIOS code):** set `VL805=1` via `rpi-eeprom-config` (from
   Raspberry Pi OS) so the firmware brings PCIe up at boot; AIOS then sees the
   controller already powered. Quickest test.
2. **AIOS mailbox request:** issue `RPI_FIRMWARE_NOTIFY_XHCI_RESET` (tag
   `0x00030058`, the call Linux makes on RPi4 to load the VL805 fw + bring up
   PCIe), possibly with `SET_DOMAIN_STATE(RPI_POWER_DOMAIN_USB=6, on)`, via the
   existing `mbox_call` (display_vc.c, channel 8) BEFORE plat_pcie_init. Confirm
   with GET_DOMAIN_STATE / a status read so a wrong call skips the controller
   access instead of crashing.
Then the brcmstb bring-up, then D.0 (the >4GB-window seL4 kernel change) for the
xHCI BAR. NEVER read a power-gated peripheral first -- it SErrors -> kernel halt.

### Gate-1 implementation (2026-06-08) -- crash-safe power confirm + opt-in probe

Implemented in `src/plat/rpi4/pcie_brcmstb.c` + `src/boot/boot_dtb.c` (HW-PENDING,
QEMU cannot exercise the brcmstb path). Two corrections to the earlier plan came
out of reading the Linux/RPi sources:

1. **`NOTIFY_XHCI_RESET` does NOT power the root complex.** Raspberry Pi firmware
   engineer (forums.raspberrypi.com t=365719): *"There is no VL805 reload
   sequence from the firmware and it expects the kernel to have done the PCI setup
   first."* The brcmstb RC is powered by the FIRMWARE AT BOOT, gated by the
   bootloader EEPROM (`VL805=1`); there is no reliable OS-side RC power-on.
   `NOTIFY_XHCI_RESET` (tag `0x00030058`, payload one u32 `dev_addr=0x100000` for
   the hardwired bus1/dev0/fn0) is the post-link call to RELOAD the VL805 device
   firmware after a PCI reset -- so it now lives on the reset path, after the link
   comes back up, NOT as a power-on.
2. **Therefore the only honest crash-safety is a real gate, not a magic mailbox.**
   AIOS cannot power the RC, so it (a) issues a best-effort VC-mailbox power request
   (`SET_DOMAIN_STATE` USB=6 on) + CONFIRM (`GET_DOMAIN_STATE` USB) -- mailbox +
   RAM only, so these CANNOT SError -- and (b) gates the actual controller MMIO
   behind a compile-time opt-in `PCIE_PROBE_LEVEL` (default 0).

What shipped:
- `boot_dtb.c parse_pcie`: re-enabled the BCM2711 fixed-address fallback (regs
  `0xFD500000`, window CPU `0x6_00000000` / PCI `0xC0000000`, bus 0-1) so
  `has_pcie=1` on RPi4 and `plat_pcie_init` runs. `#ifdef PLAT_RPI4` -> QEMU
  untouched.
- `pcie_brcmstb.c`: a self-contained VC-mailbox helper (own property buffer pinned
  LOW at `0x3A001000`, the page after display's `0x3A000000`, so the GPU-region
  device-untyped watermark stays ascending; it reuses the pre-mapped
  `dev_vcmbox_vaddr`). `pcie_power_confirm()` does SET+GET_DOMAIN_STATE and returns
  whether the firmware reports USB powered. `PCIE_PROBE_LEVEL`:
  - **0 (default, cannot brick):** mailbox request + confirm + serial log, then
    RETURN -- no controller read. The level-0 boot tells you (serial) the USB
    domain state before you ever risk `0xFD500000`.
  - **1 (opt-in):** after a positive confirm, read the controller and run the D.1
    link bring-up + VL805 config detect (`bus1 dev0` VID/PID/class; "VL805 xHCI
    DETECTED" when class==0x0C0330). On the reset path it issues NOTIFY_XHCI_RESET.
    Still returns -1 (no usable xHCI BAR -- that is D.2 / gate 2).

The USB power domain is a PROXY (it is not the RC power rail), so a level-1 boot
can still SError if the firmware left the RC gated and the domain proxy is a
false-positive -- which is why the controller read is a deliberate opt-in and the
recovery image (`disk/sdcard-rpi4.img`) must stay on hand. HW test order:
1. Flash the level-0 image (`disk/sdcard-rpi4-phaseD.img`), capture serial
   (`aios_console.py monitor /dev/cu.usbserial-0001 --baud 115200`), read the
   `[pcie] mbox GET_DOMAIN_STATE(USB) -> on=?` line. Cannot brick.
2. Set `VL805=1` in the EEPROM (`rpi-eeprom-config` on Raspberry Pi OS).
3. Rebuild `build-rpi4` with `-DPCIE_PROBE_LEVEL=1` (or edit the `#define`), remake
   the image, flash, and look for `link=UP` + `VL805 xHCI DETECTED`. If it SErrors
   ("halting... Kernel entry via Unknown"), reflash `disk/sdcard-rpi4.img`.

### Level-0 HW result (2026-06-08, serial-captured)
The level-0 image booted SAFE to login (DHCP .8, ping, SNTP, HDMI all up -- no
crash), confirming the diagnostic path. Two findings changed the design:
- **The controller IS power-gated.** The firmware boot-ROM log shows `[sdcard]
  vl805.bin not found` -> `XHCI-STOP` -> `PCI0 reset`. So a controller read now
  would SError -- EEPROM `VL805=1` is required first (as expected).
- **The VC power-domain mailbox tags are UNRELIABLE.** `SET_DOMAIN_STATE(USB,on)`
  just echoed `state=1`; `GET_DOMAIN_STATE(USB)` returned garbage (`0x3edd2d0`, not
  0/1). The original plan to GATE the controller read on a `GET_DOMAIN_STATE`
  confirm is therefore unsafe -- a garbage-nonzero value falsely reads as
  "powered". **Revised design:** the mailbox calls are INFORMATIONAL ONLY (logged
  raw); the sole gate is the deliberate `PCIE_PROBE_LEVEL=1` opt-in, set only after
  `VL805=1` makes the `XHCI-STOP` line disappear. There is no reliable OS-side
  runtime power check on this firmware.

### RE-DIAGNOSIS 2026-06-08 -- it was a reset-ORDERING bug, not a power-gate
Reading Circle (a working bare-metal Pi4 USB stack, `lib/bcmpciehostbridge.cpp`)
plus the RPi docs overturned the power-gate theory:
- **Circle does NO mailbox / clock / power call** -- it runs the brcmstb reset
  sequence and THEN reads `PCIE_MISC_REVISION`. So the core is normally readable
  bare-metal; nothing needs powering.
- **The real v0.4.183 bug:** AIOS read the PCIe-CORE registers (`MISC_REVISION`
  0x406c, `PCIE_STATUS` 0x4068) in its opening printf, BEFORE the reset sequence.
  The MISC/core block (0x4xxx) is only clocked AFTER the bridge reset is deasserted
  + SERDES IDDQ cleared. `RGR1_SW_INIT_1` (0x9210) is the always-on reset
  controller. U-Boot/Linux/Circle do the RGR1 reset dance FIRST, then read core.
  AIOS read core first -> SError. (The crash log "first read of MISC_REVISION/
  PCIE_STATUS faulted" matches exactly.)
- **`VL805=1` is COMPUTE MODULE 4 ONLY** (RPi docs, verbatim "Compute Module 4
  only") -- it does nothing on a Pi 4B and can panic if misapplied. This board (rev
  1.1, boardrev c03111) also has a DEDICATED VL805 SPI EEPROM, so the VL805 reloads
  its own firmware after our PCI reset. So the original path (a) was a dead end.

**Fix shipped:** `pcie_brcmstb.c` rewritten -- `bringup()` (RGR1 reset -> deassert
-> SERDES IDDQ) runs FIRST, then the core is read; removed the premature opening
read, the "skip reset if link up" path, and ALL the (debunked) VC-mailbox code.
`PCIE_PROBE_LEVEL` stays the opt-in gate (default 0 = no controller MMIO). NEXT HW
test (NO EEPROM change): flash the LEVEL-1 `disk/sdcard-rpi4-phaseD.img`, expect
`brcmstb ... link=UP` + `VID=1106 PID=3483 ... VL805 xHCI DETECTED`; if it SErrors,
reflash `disk/sdcard-rpi4.img`. Then gate 2 / D.2 (seL4 >4GB window + xHCI BAR).

## References

- USB xHCI 1.2 specification (usb.org)
- Linux `drivers/pci/controller/pcie-brcmstb.c`
- Linux `drivers/usb/host/xhci.c`
- Linux `drivers/hid/usbhid/hid-core.c`
- Circle bare-metal USB: `lib/usb/` (simpler reference)
- BCM2711 ARM Peripherals datasheet (section 5: PCIe)

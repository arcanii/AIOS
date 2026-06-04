# DESIGN: RPi4 Bluetooth / HCI bring-up (plan, not yet implemented)

Status: **design only.** No Bluetooth/HCI code exists in AIOS today (confirmed by
a full-tree search). This doc is the plan + effort + risks, so the work can be
picked up deliberately later. Priority: **low** -- see "Verdict."

## The hardware

The RPi4 has an onboard Cypress/Broadcom **BCM43455** combo WiFi+BT chip. The
**Bluetooth** side is a UART-attached HCI controller (model BCM4345C0) wired to
the SoC's **PL011 UART (UART0, MMIO 0xFE201000)** with 4-wire hardware flow
control (TXD/RXD/RTS/CTS on GPIO 30-33, ALT3).

Crucially for us: AIOS uses the **mini-UART (AUX, 0xFE215000, GPIO 14/15)** for
its serial console, and the **PL011 is completely unused/unclaimed** in AIOS
(`src/aios_root.c` drives only the mini-UART on RPi4; `prealloc_rpi4_devices`
in `src/boot/boot_device_map.c` never claims 0xFE201000). So a BT driver can
take the PL011 **without touching or risking the console** -- a genuinely
favorable, low-blast-radius property. (See the `rpi4-mini-uart` note.)

## Why it is non-trivial: the firmware-patch gate

The BCM4345C0 controller is **inert until patched**. After power-on it speaks a
minimal HCI, but you must download a proprietary firmware patch (`BCM4345C0.hcd`,
~40 KB, shipped by Linux `pi-bluetooth` / linux-firmware) over HCI before it is
usable. The patch cannot be synthesized; it must be obtained and embedded. The
load procedure is documented/reverse-engineered (Linux `btbcm.c`, `internalblue`)
but the blob itself is a redistribution wrinkle for a research OS.

## Bring-up steps (ordered)

1. **GPIO pinmux** -- set GPFSEL for GPIO 30/31/32/33 -> ALT3 (PL011 CTS/RTS/TXD/RXD
   to the BT chip). Distinct from the GPIO 14/15 ALT5 mini-UART console (no conflict).
   AIOS does no UART pinmux today (it relies on VC firmware for the console pins), so
   this must be written explicitly. May also need a BT_ON power/enable GPIO sequence.
2. **PL011 driver** -- claim 0xFE201000 (one `dev_req` in `prealloc_rpi4_devices`,
   ascending order), init PL011 at **115200 8N1 with RTS/CTS flow control**. AIOS has
   only the register offsets today, no PL011 driver. Polled is fine for HCI's low rate;
   flow control is mandatory once the patch stream and higher baud kick in.
3. **Firmware patchram** -- HCI_Reset, then the Broadcom vendor sequence:
   `HCI_VSC_DOWNLOAD_MINIDRIVER (0xFC2E)` -> stream `BCM4345C0.hcd` as a series of
   `HCI_VSC_WRITE_RAM (0xFC4C)` chunks -> `HCI_VSC_LAUNCH_RAM`, wait, re-open. **This is
   the load-bearing step**; without a correct patch download the controller never answers.
4. **H4 transport** -- the HCI UART framing: a packet-type byte (0x01 cmd / 0x02 ACL /
   0x04 event) + command/event matching; plus the vendor baud-change handshake
   (`HCI_VSC_UPDATE_BAUDRATE 0xFC18`) to step up to ~3 Mbaud (with flow control).
5. **Host stack** -- for anything beyond raw HCI: HCI -> L2CAP -> (SMP/ATT/GATT for BLE,
   or RFCOMM/SDP for classic). AIOS has none of this; it is a large component.

## Effort + risks

- **Milestone "HCI Reset + Read_Local_BD_ADDR over H4": MEDIUM** (a few focused days),
  mostly self-contained since the PL011 is free. The handful of HCI byte sequences is
  easy; getting past the firmware-patch gate (step 3) is the hard part.
- **Risks:** (a) the proprietary `.hcd` blob (must bundle; redistribution wrinkle);
  (b) hardware flow control on GPIO 30/31 is mandatory at high baud -- a polled, no-flow
  PL011 will drop bytes mid-`.hcd` (~40 KB) and the patch silently fails; (c) no PL011
  DMA in AIOS (PIO fine for HCI rates); (d) power/enable sequencing is easy to get
  subtly wrong with no Linux `pi-bluetooth` underneath.

## Verdict + recommendation

**Low priority / not worth it right now.** A bare "read BD_ADDR" is a cute milestone but
a dead end without the L2CAP/GATT stack (step 5 = large), and it depends on a proprietary
blob. Versus the working network path (GENET Ethernet + DHCP + the netconsole control
channel + file transfer + SNTP), Bluetooth delivers far less per unit effort.

**If pursued later, suggested phasing** (each independently testable, console-safe):
- Phase 1: PL011 driver + GPIO ALT3 pinmux -> loopback/raw-byte test (no BT chip logic).
- Phase 2: H4 framing + HCI_Reset (expect a command-complete event -- proves the link).
- Phase 3: embed `BCM4345C0.hcd` + patchram loader -> Read_Local_BD_ADDR returns the
  real address (proves the controller is alive).
- Phase 4 (large, separate effort): a minimal BLE stack (L2CAP/ATT/GATT) for an actual use.

Stop after Phase 3 unless there is a concrete BLE/classic use case driving Phase 4.

## References
- rpi4os.com Part 7 -- bare-metal RPi4 Bluetooth from scratch (the closest reference).
- Linux `drivers/bluetooth/btbcm.c`, `hci_bcm.c` -- the patchram + baud sequence.
- `internalblue` -- BCM firmware/patchram internals.
- See also `docs/DESIGN_USB_HID.md` (the other not-started input-device effort).

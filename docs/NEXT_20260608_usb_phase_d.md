# NEXT 2026-06-08 -- USB HID Phase D: bring up RPi4 PCIe -> VL805 xHCI on real HW

Seed for a fresh session. Read with `HANDOVER.md`, `docs/DESIGN_USB_HID.md` (the
full design + the "Phase D findings" / "HW RESULT" sections), and the
`project_usb_hid` memory. The USB HID stack is QEMU-COMPLETE (a USB keyboard types
into the AIOS shell on QEMU); this is the last layer -- the RPi4 hardware bring-up.

> NOTE: everything below the "## FINAL STATUS" block is the original seed + a
> chronological RE log kept for reference. The FINAL STATUS block supersedes it
> (several early theories there -- VC-mailbox power-on, EEPROM VL805=1 -- were
> DISPROVEN on hardware; do not re-try them).

## FINAL STATUS 2026-06-08 -- PHASE D.1 COMPLETE ON REAL HARDWARE -- READ THIS FIRST

**Result: the RPi4 brcmstb PCIe link trains AND the VL805 xHCI ENUMERATES on the real
Pi.** Serial (PROBE_LEVEL=1, `src/plat/rpi4/pcie_brcmstb.c`):
```
[pcie] brcmstb rev=0x0304 PCIE_STATUS=0xb0 link=UP mode=RC
[pcie] SSC: ssc=1 pll_lock=1
[pcie] link: 5.0 Gbps x1 (lnksta=0x9012)
[pcie] bus1 dev0: VID=1106 PID=3483 class=0c0330
[pcie] VL805 xHCI DETECTED -- Phase D.1 link OK.
```
~16 HW boots, SEVEN root-caused bugs. The actual keyboard is now D.2 (the xHCI BAR
via a seL4 >4GB device window) -- see "REMAINING" below.

Seven bugs found + fixed this session, each confirmed by the next boot:
1. **Boot SError "Kernel entry via Unknown"** = a RESET-ORDERING bug, NOT a power-
   gate: the old code read PCIe-core regs (MISC_REVISION 0x406c / PCIE_STATUS 0x4068)
   BEFORE the reset. Those are clocked only after the RGR1 (0x9210, always-on)
   bridge-reset deassert + SERDES IDDQ clear. Fix: RGR1 reset dance FIRST, then read.
2. **link=DOWN** = inverted PERST polarity. bcm2711 RGR1_SW_INIT_1 bit0: 1=assert
   (hold EP in reset), 0=deassert (release). We had it backwards. Fix: setb to
   assert early, clrb to release at the end.
3. CRS handling: settle + retry the vendor read after link-up.
4. **NOTIFY_XHCI_RESET is a RESET/handoff call, NOT a fw loader** (Linux commit
   fdb3db3: called from quirk_usb_handoff_xhci only when vendor==VIA && dev==0x3483,
   i.e. the device must ALREADY enumerate). Returns "ok" but does nothing for a
   0xffff device. Left in as a harmless no-op; can be removed with the mailbox helper.
5. Outbound MEM window programmed (CPU 0x6_00000000 -> PCI 0xC0000000, 1GB) before
   the bus scan -- U-Boot does this; encoding from Linux brcm_pcie_set_outbound_win
   (base in BASE_LIMIT[15:4] 0xfff0, limit in [31:20] 0xfff00000, high >>12).
6. SSC (Spread Spectrum Clocking) via the internal MDIO bus -- `set_ssc` + `mdio_read/
   write` ported from u-boot. Link now trains 5.0 Gbps x1 (lnksta=0x9012), matching
   U-Boot exactly. (Plus U-Boot parity: MISC RCB_MPS_MODE 0x400, MSI INTR2 mask/clear
   0x4510/0x4508, endian VENDOR_SPECIFIC_REG1 0x0188, ASPM-disable PRIV1_LINK_CAP 0x04dc.)
7. **THE FINAL FIX -- RC bridge BUS-NUMBER forwarding.** SSC + a U-Boot-identical link
   still gave config 0xffff. The brcmstb will not forward a config TLP to bus 1 unless
   the RC's secondary bus = 1. U-Boot's GENERIC PCI CORE sets this during enumeration
   (the driver probe does NOT -- invisible if you only read the driver). Fix (one write,
   before the config read): `wr(RC_PRIMARY_BUS=0x18, pri=0 | sec<<8=1 | sub<<16=1)` +
   enable RC_COMMAND (0x04) mem+busmaster. RC config header is at base+offset. -> the
   VL805 enumerates (VID=1106 PID=3483).

**Why D.1 took so long (the lesson):** the brcmstb config path needs both (a) the
driver bring-up (link) AND (b) the generic-PCI-core bridge setup (bus numbers). We
cloned the driver perfectly but missed (b) -- which lives OUTSIDE the controller
driver. When porting a Linux/U-Boot controller driver to a from-scratch OS, port the
PCI-core bridge config (sec/sub bus, command reg) too, not just the controller probe.

**Earlier wrong turn (now resolved):** before the bus-number fix this looked like a
seL4 environmental wall. It was not -- we read the LOCAL U-Boot tree (`../u-boot` v2026.07,
`drivers/pci/pcie_brcmstb.c` + `arch/arm/mach-bcm283x/include/mach/acpi/bcm2711.h`)
and verified our driver is a FAITHFUL CLONE. Config access is BYTE-IDENTICAL:
`brcm_pcie_config_address` passes where=0 to `PCIE_ECAM_OFFSET` so
idx = bus<<20 | dev<<15 | fn<<12, then reads at `EXT_CFG_DATA + offset` -- exactly
our `cfg_rd`. Reset/PERST/SERDES/link/inbound/outbound all match. The VL805 PHY
trains (link=UP) but its config/transaction layer never responds (UR -> 0xffff) =
it is not running firmware in AIOS's post-firmware boot state, and no driver change
moves it.

DISPROVEN this session (do NOT re-try): the firmware power-gate theory (it was the
reset-ordering bug); the VC-mailbox power-on (SET/GET_DOMAIN_STATE -- GET returned
garbage 0x3edd2d0); EEPROM `VL805=1` (Compute Module 4 ONLY per RPi docs, irrelevant
to a Pi 4B); newer RPi firmware (master start4.elf 2305632 -- identical result);
PERST-preserve-vs-reset (gentle and full both 0xffff); the outbound window alone;
the U-Boot-parity steps alone.

### GROUND TRUTH DONE -- U-Boot ENUMERATES the VL805 on this Pi (it is our seL4 env)
Ran the user's prebuilt u-boot (`../u-boot`, rpi_4_defconfig, CONFIG_PCI_BRCMSTB=y;
staged at `disk/u-boot-rpi4.bin`, booted by copying it as kernel8.img on AIOSBOOT).
U-Boot serial: `PCIe BRCM: link up, 5.0 Gbps x1 (SSC)` + `USB XHCI 1.00 / Bus
xhci_pci: 2 USB Device(s) found`. So the BOARD + VL805 WORK and our driver (a
faithful clone) is CORRECT -- the wall is a seL4 ENVIRONMENTAL difference, NOT the
chip and NOT our logic. This is now a focused, fixable bug.

**RULED OUT:** the device-MMIO memory type -- seL4 maps `cacheable=0` as DEVICE_nGnRnE
(`deps/kernel/src/arch/arm/64/kernel/vspace.c:698`), IDENTICAL to U-Boot bare-metal;
`arch_dsb()` = `dsb sy`. So not mapping/barriers/speculation.

portable wall (the SSC + bus-number fixes above closed it). Restore AIOS after a
U-Boot test: copy `disk/kernel8.img` (or `kernel8-recovery.img`) back as kernel8.img.

### REMAINING -> D.2 (the actual keyboard)
D.1 (PCIe + VL805 detection) is DONE on HW. To make a USB keyboard type, finish D.2:
1. In `pcie_bringup_and_detect` (after the VL805 is detected), program the VL805 BAR0
   in the PCI MMIO window (size it, place it at `pcie_mmio_pci`=0xC0000000+, enable it
   via the device's command reg), compute the CPU-side address
   `pcie_xhci_bar = 0x6_00000000 + (bar_pci - 0xC0000000)`, set `pcie_xhci_bus/dev/fn`
   and `pcie_xhci_present = 1`.
2. seL4 KERNEL CHANGE (D.0): the BAR is at CPU 0x6_00000000, ABOVE seL4 bcm2711's 4GB
   device-untyped top, so `sel4platsupport_alloc_frame_at` cannot map it. Extend the
   bcm2711 kernel device regions to expose the PCIe outbound window >4GB
   (`deps/kernel/src/plat/bcm2711` hw spec -> devices_gen.h, bump max paddr), kernel
   rebuild (gitignored deps/ -- re-apply on reset). Add the window to
   `prealloc_rpi4_devices` (ascending paddr).
3. Then `xhci_init()` (`src/usb/xhci.c`, Layers 2-5, already QEMU-verified) maps the
   BAR + runs the controller; the existing polling driver thread feeds keys via
   SER_KEY_PUSH. A72 ring coherency is QEMU-invisible -- verify on HW (DMA rings are
   non-cacheable + paddr; Unify_Instruction not needed, no code-via-data here).
4. Bump version.h -> 0.4.184 + README when a key types on HW; commit.
Cleanup (optional): remove the NOTIFY_XHCI_RESET call + the VC-mailbox helper (no-op
-- U-Boot omits it; the VL805 self-loads from its dedicated EEPROM on PERST).

### Repo state (uncommitted; commit only when asked)
- `src/plat/rpi4/pcie_brcmstb.c` -- the full brcmstb driver, PROBE_LEVEL default 0
  (safe: no controller MMIO, cannot SError). Build with -DPCIE_PROBE_LEVEL=1 (or edit
  the `#define`) to probe.
- `src/boot/boot_dtb.c` -- RPi4 fixed-address fallback (`#ifdef PLAT_RPI4`) sets
  has_pcie so plat_pcie_init runs. QEMU untouched.
- `scripts/mkkernel8.py` -- NEW: flash-free kernel updates (kernel8.img -> FAT,
  [[feedback_flashfree_kernel]]). `disk/kernel8.img` = the PROBE_LEVEL=1 test build;
  `disk/kernel8-recovery.img` = the safe PROBE_LEVEL=0 build. Recovery full image
  `disk/sdcard-rpi4.img`.
- QEMU unaffected: smp_qemu_test 7/7, xhci_key_qemu_test PASS. version stays 0.4.183
  (bump only when USB types on HW). The NOTIFY + VC-mailbox helper in pcie_brcmstb.c
  is now a no-op and can be removed on cleanup.

## TL;DR -- the one thing to build
Get the RPi4 PCIe controller + VL805 xHCI alive so the existing shared xHCI driver
(`src/usb/xhci.c`) runs on real hardware -> a USB keyboard types into the AIOS
shell on the Pi. Two gates, in order: (1) POWER ON the controller (the firmware
power-gates it at OS handoff) via a VC-mailbox request, then (2) expose the PCIe
MMIO window to seL4 (it is above the 4GB device-untyped top) so the xHCI BAR maps.

## Where things are (committed, v0.4.183, branch main)
- **Phase 2 shared .text: HW-VERIFIED.** Done.
- **USB HID A/B/C: QEMU-complete + committed.** PCIe ECAM
  (`src/plat/qemu-virt/pcie_ecam.c`), xHCI init + enumeration + HID + keymap +
  polling driver thread (`src/usb/xhci.c`), fed to tty via SER_KEY_PUSH. Test:
  `python3 scripts/xhci_key_qemu_test.py` (PASS). Layers 2-5 are
  platform-independent; only Layer 1 (PCIe) differs by platform.
- **Phase D (RPi4): BLOCKED, code present but safe.** `src/plat/rpi4/pcie_brcmstb.c`
  has the brcmstb bring-up + VL805 config-detect (D.1). `boot_dtb.c parse_pcie`
  currently leaves `has_pcie=false` on RPi4 (the fixed-address fallback is
  reverted) so `plat_pcie_init` is SKIPPED and the boot is safe. `dev_pcie_vaddr`
  (controller regs, 10 pages @0xFD500000) is wired into `prealloc_rpi4_devices`
  (only claimed when has_pcie). The xHCI driver only runs when `pcie_xhci_present`.

## The two gates (confirmed by HW testing -- see DESIGN_USB_HID.md HW RESULT)
**Gate 1 -- the firmware power-gates the PCIe controller.** Reading 0xFD500000
from AIOS SError'd -> kernel halt (the controller is powered down at handoff; the
pcie node is "present" in the DTB but its ranges does not parse). vl805.bin is NOT
needed (it is an EEPROM-update blob; the VL805 fw is in the bootloader EEPROM).
The VL805/PCIe is "loaded on request from the kernel" -- so AIOS must REQUEST it.
Unblock with ONE of:
  (a) **Quickest test, no AIOS code:** set `VL805=1` in the bootloader EEPROM
      config (`rpi-eeprom-config` from Raspberry Pi OS), so the firmware brings
      PCIe up at boot; AIOS then sees the controller already powered.
  (b) **Self-contained:** issue `RPI_FIRMWARE_NOTIFY_XHCI_RESET` (mailbox tag
      0x00030058 -- the call Linux makes on RPi4 to load the VL805 fw + bring up
      PCIe), maybe + `SET_DOMAIN_STATE(RPI_POWER_DOMAIN_USB=6, on)` (tag 0x38030),
      via the existing `mbox_call` in `src/plat/rpi4/display_vc.c` (channel 8,
      buffer @0x3A000000), BEFORE `plat_pcie_init`.
**Gate 2 -- the PCIe MMIO window is outside seL4's address space.** The window is
CPU 0x6_00000000 (DTB ranges), but seL4 bcm2711
(`build-rpi4/kernel/gen_headers/plat/machine/devices_gen.h`) exposes only RAM <4GB
+ a few device frames. So `sel4platsupport_alloc_frame_at` cannot map the xHCI BAR.
Fix: extend the seL4 bcm2711 device regions to include the PCIe window above 4GB
(`deps/kernel/src/plat/bcm2711/overlay-rpi4.dts` / the kernel hardware spec ->
devices_gen.h; ensure max paddr covers it), kernel rebuild. (Gitignored deps/ --
re-apply on a deps reset.) NOTE: D.1 link bring-up + VL805 config-detect needs
ONLY gate 1 (config access uses the controller regs, not the window); the BAR
needs gate 2.

## Recommended order
1. Try gate-1 path (a): flip `VL805=1` on the EEPROM, re-enable the fixed-address
   fallback in `boot_dtb.c parse_pcie` (it is reverted; re-add the BCM2711 block),
   reflash, watch serial. If the controller now reads (no SError) and the brcmstb
   bring-up logs `[pcie] ... link=UP` + `bus1 dev0: VID=1106 ... VL805 DETECTED`,
   gate 1 is solved -> D.1 confirmed.
2. If (a) is not viable, implement gate-1 path (b) (mailbox NOTIFY_XHCI_RESET).
   Make it CRASH-SAFE: confirm via a status read / GET_DOMAIN_STATE before any
   controller MMIO, so a wrong call skips rather than halts.
3. Then gate 2 (seL4 window) + re-enable the xHCI BAR map in pcie_brcmstb.c (D.2),
   reuse `src/usb/xhci.c` Layers 2-5 unchanged -> keyboard on HW. Bump version.h +
   README when it lands.

## Critical gotchas (these WILL bite)
- **NEVER read a power-gated BCM2711 peripheral.** It SErrors -> "halting... Kernel
  entry via Unknown" -> kernel halt -> Pi unreachable -> reflash to recover. Do
  gate 1 (power-on) and confirm BEFORE any controller MMIO read.
- **SERIAL IS ESSENTIAL.** The `[pcie]` detection lines are printf -> mini-UART
  serial only (NOT /proc/log, which is a ring that rotates past boot). Capture with
  `python3 scripts/aios_console.py monitor /dev/cu.usbserial-0001 --baud 115200`
  (USB-serial on GPIO14/15). Without it you are blind.
- **QEMU cannot model brcmstb** -- the RPi4 PCIe path is HW-only. Keep
  `scripts/smp_qemu_test.py` at 7/7 (QEMU is unaffected by RPi4-only changes).
- **Drive the Pi gently over netconsole** (it can wedge); this Pi's netconsole is
  the "one command per line" variant. DHCP gives .8 (real MAC) or .127 (fallback).
- A safe, bootable recovery image is at `disk/sdcard-rpi4.img` (current main:
  plat_pcie_init skipped). Regenerate with `python3 scripts/mksdcard.py` after a
  build. Reflash via BalenaEtcher (root-task change -> no push-over-net).

## Key files
- `src/boot/boot_dtb.c` (parse_pcie -- re-add the BCM2711 fixed-address block)
- `src/plat/rpi4/pcie_brcmstb.c` (bring-up + VL805 detect; add the mailbox power-on)
- `src/plat/rpi4/display_vc.c` (mbox_call -- reuse for the mailbox request)
- `src/usb/xhci.c` (shared Layers 2-5; D.2 sets pcie_xhci_* + pcie_xhci_present)
- `src/boot/boot_device_map.c` / `include/aios/device_map.h` (dev_pcie_vaddr)
- `include/aios/pcie.h`, `include/aios/xhci.h`

## References
- rpi-eeprom firmware-2711 release-notes; RPi booteeprom docs (VL805=1 / vl805.bin).
- U-Boot/Linux pcie-brcmstb (bring-up sequence + offsets -- in DESIGN_USB_HID.md).
- RPI_FIRMWARE_* mailbox tags (Linux include/soc/bcm2835/raspberrypi-firmware.h).

## Conventions
- Develop/verify on QEMU first where possible; the brcmstb path is HW-only (flash +
  serial). No apostrophes in C comments. Commit only when asked. version.h ->
  0.4.184 when USB works on HW. Commit msgs end with:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

## Fallback
Phase D is purely additive -- if it stalls, the USB stack stays QEMU-complete and
the system is HW-verified at v0.4.183. No pressure to land it in one session; it
is a multi-flash HW effort. Bank each gate as it is proven.

## RE-DIAGNOSIS 2026-06-08 (SUPERSEDES the gate-1/power-on framing below)

The "gate 1 = firmware power-gate, fix via VL805=1 / VC-mailbox" framing in this
doc and the original seed is WRONG. After the level-0 HW boot + reading Circle
(working bare-metal Pi4 USB) + the RPi docs:
- The v0.4.183 SError was a **reset-ORDERING bug**: AIOS read PCIe-core regs
  (MISC_REVISION 0x406c, PCIE_STATUS 0x4068) in its opening printf, BEFORE the
  reset. Those MISC/core regs (0x4xxx) are only clocked AFTER the bridge reset is
  deasserted + SERDES IDDQ cleared; RGR1_SW_INIT_1 (0x9210) is the always-on reset
  controller. Circle/U-Boot/Linux run the RGR1 reset dance FIRST, then read core.
- **`pcie_brcmstb.c` rewritten**: bringup() (RGR1 reset -> deassert -> SERDES IDDQ)
  runs FIRST, then the core is read. Removed the premature opening read + the "skip
  reset if link up" path. Removed ALL the VC-mailbox code (debunked).
- **DEBUNKED, do not re-try:** VC-mailbox power-on (Circle uses none;
  GET_DOMAIN_STATE returned garbage on HW) and EEPROM `VL805=1` (it is COMPUTE
  MODULE 4 ONLY per the RPi docs -- irrelevant to the Pi 4B; and this board, rev
  1.1 / boardrev c03111, has a DEDICATED VL805 SPI EEPROM that self-reloads after
  our PCI reset).
- **NEXT TEST (no EEPROM change):** `disk/sdcard-rpi4-phaseD.img` is now a LEVEL-1
  image (corrected reset-first ordering). Flash it, capture serial, expect:
  `[pcie] brcmstb @ ... core bring-up first`, then `[pcie] brcmstb rev=0x.. link=UP`,
  then `[pcie] bus1 dev0: VID=1106 PID=3483 ...` + `VL805 xHCI DETECTED`. If it
  SErrors (`Kernel entry via Unknown`), reflash `disk/sdcard-rpi4.img`. Source
  default stays `PCIE_PROBE_LEVEL=0` (safe). Then gate-2/D.2 (seL4 >4GB window +
  xHCI BAR). Sources: Circle `lib/bcmpciehostbridge.cpp`; RPi `eeprom-bootloader.adoc`.

### LEVEL-1 HW result #1 2026-06-08 -- crash SOLVED, found a PERST polarity bug
Flashed the level-1 image: **no SError** -- the core read works. Serial:
`[pcie] brcmstb rev=0x0304 PCIE_STATUS=0x80 link=DOWN mode=RC`. So gate 1 (the
crash) is solved: the core is alive, readable, RC mode. But the link did not train
(0x80 = PORT/RC set; DL_ACTIVE 0x20 + PHYLINKUP 0x10 both clear). Root cause: the
PERST polarity was INVERTED. For bcm2711 `RGR1_SW_INIT_1` bit 0, **1 = assert PERST
(hold the EP in reset), 0 = deassert (release)** -- per U-Boot `brcm_pcie_perst_set`
+ Linux `brcm_pcie_perst_set_generic`. The old code released PERST early and
ASSERTED it at the end, holding the VL805 in reset forever. **Fixed** (bringup():
`setb` to assert early, `clrb` to release at the end). The level-1
`disk/sdcard-rpi4-phaseD.img` was rebuilt with the fix. **Next HW test:** reflash,
expect `link=UP` + `VID=1106 PID=3483` + `VL805 xHCI DETECTED`.

## STATUS 2026-06-08 -- gate-1 AIOS side IMPLEMENTED (HW test pending) [SUPERSEDED above]

Built + QEMU-regression-clean (smp 7/7); the brcmstb path is HW-only so the Pi
flash is the next step (NOT done -- needs the physical Pi + serial). UNCOMMITTED
(commit when asked); version stays 0.4.183 (bump to 0.4.184 only when USB types on
HW). Two source files changed, both reviewed:

- `src/boot/boot_dtb.c parse_pcie`: re-enabled the BCM2711 fixed-address fallback
  (`#ifdef PLAT_RPI4` -> QEMU untouched) so `has_pcie=1` on RPi4 and
  `plat_pcie_init` runs.
- `src/plat/rpi4/pcie_brcmstb.c`: rewritten for crash-safety.
  - A self-contained VC-mailbox helper (own property buffer at `0x3A001000`, the
    page after display's `0x3A000000`; reuses pre-mapped `dev_vcmbox_vaddr`).
  - `pcie_mbox_power_info()` = SET_DOMAIN_STATE(USB,on) + GET_DOMAIN_STATE(USB),
    mailbox-only so it CANNOT SError. INFORMATIONAL ONLY (logged raw) -- it does
    NOT gate (see the level-0 result below).
  - `PCIE_PROBE_LEVEL` opt-in (default **0**): level 0 does the mailbox log and
    RETURNS (no `0xFD500000` read -> cannot brick); level 1 reads the controller +
    runs the D.1 link/VL805 detect. The opt-in itself is the gate.
  - NOTIFY_XHCI_RESET moved to the post-link reset path (it reloads the VL805
    device fw; per RPi fw it does NOT power the RC -- the RC power-on is the EEPROM
    `VL805=1`, a firmware/boot action AIOS cannot do).

**Key correction vs the seed above:** path (b) "AIOS mailbox NOTIFY_XHCI_RESET as
the power-on" is WRONG -- the RPi firmware (forum t=365719) says it "expects the
kernel to have done the PCI setup first," i.e. it does not power the RC. There is
no OS-side RC power-on. So gate-1 = EEPROM `VL805=1` (path a, the user's action);
AIOS's contribution is the crash-safe guard + the fixed-address fallback + the
post-link VL805 fw reload.

### Level-0 result (DONE 2026-06-08, serial-captured)
Level-0 flash booted SAFE to login (DHCP .8, ping, SNTP, HDMI -- no crash). Two
findings: (1) the controller IS power-gated -- firmware boot-ROM shows `[sdcard]
vl805.bin not found` -> `XHCI-STOP` -> `PCI0 reset`, so a read now would SError;
(2) the VC power-domain tags are UNRELIABLE -- `SET_DOMAIN_STATE(USB,on)` echoed
`state=1`, `GET_DOMAIN_STATE(USB)` returned garbage `0x3edd2d0`. So the mailbox
confirm was demoted to informational and the PROBE_LEVEL opt-in is the only gate
(code already updated). Next: EEPROM `VL805=1`, then re-check, then level 1.

### HW test procedure (do these on the Pi, in order)
1. DONE -- level-0 baseline (above): safe boot, XHCI-STOP present, tags garbage.
2. **Set `VL805=1` in the bootloader EEPROM** on Raspberry Pi OS:
   `rpi-eeprom-config --edit` (add a `VL805=1` line), reboot RPi OS once to apply.
3. **Re-flash the level-0 image** (`disk/sdcard-rpi4-phaseD.img`, still safe -- no
   controller read), capture serial, and confirm the firmware boot-ROM `XHCI-STOP`
   / `vl805.bin not found` lines are now GONE (i.e. the firmware powered PCIe). If
   they are still there, `VL805=1` did not take -- do NOT go to level 1.
4. **Probe (level 1):** edit `#define PCIE_PROBE_LEVEL 0` -> `1` in
   `src/plat/rpi4/pcie_brcmstb.c` (or build with `-DPCIE_PROBE_LEVEL=1`), `ninja`
   in build-rpi4, `python3 scripts/mksdcard.py --output disk/sdcard-rpi4-phaseD.img`,
   flash, serial. SUCCESS = `[pcie] PCIE_STATUS=... link=UP ...` +
   `[pcie] bus1 dev0: VID=1106 PID=3483 ...` + `VL805 xHCI DETECTED`. That is gate
   1 + D.1 PROVEN. If it halts (`Kernel entry via Unknown`), the RC was still gated
   -- reflash `disk/sdcard-rpi4.img`, recheck step 2/3.
5. **Then gate 2 / D.2:** extend the seL4 bcm2711 kernel to expose the PCIe MMIO
   window >4GB, map the xHCI BAR in `pcie_bringup_and_detect`, set `pcie_xhci_*` +
   `pcie_xhci_present`, reuse `src/usb/xhci.c` Layers 2-5 -> keyboard on HW. Then
   bump version.h -> 0.4.184 + README.

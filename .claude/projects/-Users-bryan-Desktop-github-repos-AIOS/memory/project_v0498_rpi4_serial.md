---
name: v0.4.98 RPi4 serial boot
description: RPi4 boots to interactive login via USB-serial on GPIO 14/15, relocator stub required
type: project
---

RPi4 boots to interactive serial login as of v0.4.98.

**What changed since v0.4.93**: SMP (KernelMaxNumNodes 4, reverted to 1), GENET driver (crashes, disabled), display driver (VKA assert, disabled), ARM64 Image header now required by firmware, firmware relocates to 0x80000 (need relocator stub).

**Boot chain**: RPi firmware -> ARM64 Image header check -> relocate to 0x80000 -> 4KB relocator stub copies seL4 to 0x370c000 -> seL4 elfloader -> kernel -> root task -> mini UART serial -> getty -> login

**Key fixes applied**:
- KernelMaxNumNodes 4 -> 1 (SMP crashes boot)
- ARM64 Image header with "ARMd" magic at offset 0x30
- 4KB relocator stub (firmware always relocates to 0x80000)
- GENET driver disabled (crashes during MMIO mapping)
- Display driver disabled (VKA allocator assert in VC mailbox init)
- No dtoverlay=disable-bt (GPIO 14/15 must be mini UART for root task)
- Method 2 FAT32 (diskutil) -- mtools FAT32 breaks RPi firmware
- MBR manually patched for ext2 P2 partition

**Known issues**: RAM shows -217 MB (unsigned overflow in DTB parsing), banner shows v0.4.96/build 1528 (stale ext2 binaries)

**Why:** RPi4 is Priority 1. Serial debug changes the workflow from HDMI-only to proper console.
**How to apply:** Use hw/rpi4/BOOT_NOTES.md for flash procedure. Relocator stub must be regenerated when image size changes (entry point shifts).

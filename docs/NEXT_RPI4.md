# AIOS RPi4 Branch -- v0.4.95

Branch `rpi4` from main at v0.4.94. RPi4 hardware-specific development.

## Features Implemented (v0.4.95)

### 1. SD Card Write Support (blk_emmc.c)

**Previous state:** Read-only PIO (CMD17 single-block reads).

**Added:** `emmc_write_raw_sector()` -- CMD24 WRITE_SINGLE_BLOCK via PIO.
Mirrors the read path: wait CMD done, wait BUF_WR (buffer write ready),
write 128 x 32-bit words to SDHCI data port, wait XFER_DONE.
Uses the same aligned `sector_buf` as reads.

`plat_blk_write()` now adds partition offset, matching the read path.
ext2 write operations (file create, append, mkdir) are now functional
on RPi4 hardware.

### 2. Splash Screen Timing (boot_display_init.c, fb_console.c)

**Problem:** `fb_console_init()` immediately cleared the entire framebuffer
after splash render, making the splash invisible.

**Fix:**
- Added ~2 second spin delay after splash render before `fb_console_init()`
- `fb_console_init()` now preserves top 8 text rows (80px header area)
- Console text starts at row 8, only clears pixels below the header
- Splash banner + version text remain visible during boot

### 3. GENET Ethernet Driver (net_genet.c -- 738 lines)

Full BCM54213 GENET v5 Ethernet driver replacing the 28-line stub.

**MAC init:**
- Maps 64KB (16 pages) register region from `hw_info.genet_paddr`
- UniMAC software reset, reads MAC from UMAC_MAC0/MAC1 (firmware-programmed)
- Configures RBUF (2-byte alignment, discard bad frames)
- Sets port mode to external GPHY

**MDIO PHY (BCM54213):**
- `mdio_read()`/`mdio_write()` via UMAC_MDIO_CMD register
- PHY ID verification
- Phase 1: forced 100Mbps full duplex (no autoneg)
- Link-up wait with timeout

**DMA:**
- 128KB DMA region (size-17 untyped, 32 pages)
- GENET descriptor rings: 16 RX + 16 TX (ring 16 = default queue)
- Each descriptor: length_status + addr_lo + addr_hi (12 bytes)

**TX:** `plat_net_tx()` -- copy frame to DMA buffer, write descriptor with
SOP|EOP|CRC flags, advance producer index.

**RX:** `plat_net_driver_fn()` -- IRQ-driven thread, drains completed RX
descriptors into `net_rx_ring` (same SPSC ring as virtio-net), strips
2-byte alignment padding + 4-byte CRC, recycles descriptors.

**IRQ:** `simple_get_IRQ_handler` + `seL4_IRQHandler_SetNotification`,
same pattern as virtio-net.

### 4. SMP -- 4-Core Spin-Table Wakeup

**Previous state:** `KernelMaxNumNodes 1` (single core only).
Diagnostic stub parked secondary cores in dead `wfe` loop.

**Fix:**
- `settings-rpi4.cmake`: `KernelMaxNumNodes` 1 -> 4
- `diag_entry.S`: Secondary cores now implement firmware spin-table protocol:
  - Compute `cpu-release-addr = 0xd0 + core_id * 8`
    (core 1=0xd8, core 2=0xe0, core 3=0xe8 -- matching rpi4.dts)
  - Clear release address, spin in `wfe` + load loop
  - When elfloader writes `secondary_startup` and issues `sev`, core jumps
  - seL4 kernel `release_secondary_cpus()` handles the rest

## Modified Files

| File | Change |
| --- | --- |
| include/aios/version.h | 94 -> 95 |
| settings-rpi4.cmake | KernelMaxNumNodes 1 -> 4 |
| src/plat/rpi4/blk_emmc.c | CMD24 write (+79 lines) |
| src/plat/rpi4/net_genet.c | Full GENET driver (+731 lines) |
| src/plat/rpi4/diag_stub/diag_entry.S | Spin-table park (+32/-6 lines) |
| src/boot/boot_display_init.c | Splash delay (+7 lines) |
| src/boot/fb_console.c | Header preservation (+14/-8 lines) |

## Testing Status

| Feature | QEMU | RPi4 Hardware |
| --- | --- | --- |
| SD card write (CMD24) | N/A (virtio) | **Untested** |
| Splash timing | Testable | **Untested** |
| GENET Ethernet | N/A (virtio) | **Untested** |
| SMP (4 cores) | Already works | **Untested** |

All features require RPi4 hardware validation before merge to main.

## Next Steps (Priority Order)

1. **Hardware test** -- Build RPi4 target, flash SD, verify all 4 features
2. **GENET autoneg** -- Replace forced 100Mbps with proper auto-negotiation
3. **GENET 1000Mbps** -- Enable gigabit once autoneg works
4. **USB HID keyboard** -- PCIe root complex + xHCI for HDMI keyboard input
5. **Update disk binaries** -- Cross-compile getty/dash for Cortex-A72
6. **SSH server** -- With GENET working, implement minimal SSH for remote access

## Key References

| Component | Reference |
| --- | --- |
| BCM2711 GENET | Linux `drivers/net/ethernet/broadcom/genet/` |
| BCM54213 PHY | Linux `drivers/net/phy/broadcom.c` |
| SDHCI write | SD Physical Layer Spec, SDHCI 3.0 Spec |
| Spin-table SMP | seL4 `elfloader-tool/src/arch-arm/drivers/smp-spin-table.c` |
| Circle bare-metal | `github.com/rsta2/circle` (RPi4 GENET, SDHCI) |

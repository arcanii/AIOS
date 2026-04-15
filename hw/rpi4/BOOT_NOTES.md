# RPi4 Boot Notes

Verified working: v0.4.98, build 1531, 2026-04-15

## Hardware
- Board: Raspberry Pi 4 Model B (BCM2711, 4x Cortex-A72)
- Revision: c03111 (4GB RAM)
- Serial: USB-to-serial adapter on GPIO 14/15 (mini UART, 115200 baud)
- EEPROM: 2026-02-23

## Working Boot Chain

1. RPi firmware (start4.elf) loads kernel8.img from FAT32 partition
2. Firmware checks ARM64 Image header (magic "ARMd" at offset 0x30)
3. Firmware ALWAYS relocates ARM64 images to 0x80000 (ignores kernel_address)
4. 4KB relocator stub at 0x80000 copies seL4 payload to link address
5. seL4 elfloader boots kernel (printf disabled for BCM2711)
6. Kernel drops to root task
7. Root task maps mini UART at 0xFE215000, starts serial I/O
8. Auth server, getty, dash shell -- interactive login

## Critical Requirements

### ARM64 Image Header (offset 0x00-0x3F)
- code0 at 0x00: "b #64" (0x14000010) -- branch past 64-byte header
- magic at 0x30: 0x644d5241 ("ARMd")
- Without magic: firmware rejects with "No compatible kernel found"
- image_size at 0x10: total file size (stub + payload)
- text_offset at 0x08: ignored by firmware (set to 0)

### Relocator Stub (4KB at offset 0x000-0xFFF)
- Firmware ALWAYS relocates to 0x80000 regardless of config.txt settings
- seL4 elfloader linked at high address (~0x370c000, changes with image size)
- If seL4 runs at 0x80000: kernel load overwrites elfloader -> crash
- Stub uses ADR (PC-relative) to find itself, copies payload to link address
- Preserves x0 (DTB address from firmware) across copy
- Flushes I-cache after copy: ic iallu, dsb sy, isb
- Source: hw/rpi4/relocator.S (regenerated per build, entry address changes)

### FAT32 Boot Partition
- RPi firmware FAT32 parser rejects mtools-generated (mformat/mcopy) FAT32
- Symptom: "dterror: Failed to load Device Tree file '?'" then no kernel
- MUST use macOS diskutil to create FAT32 (Method 2)
- Never dd an mtools/mksdcard.py FAT32 image to the SD card

### UART Configuration
- WITHOUT dtoverlay=disable-bt: GPIO 14/15 = mini UART (ALT5) -- CORRECT
- WITH dtoverlay=disable-bt: GPIO 14/15 = PL011 -- WRONG for root task
- Root task uses mini UART (AUX registers at 0xFE215000)
- core_freq=250 + core_freq_min=250: stabilizes mini UART baud rate
- Elfloader printf DISABLED for BCM2711 (mini UART hangs CPU bus in elfloader)
- First serial output appears when root task maps UART (~2s after firmware)

### SD Card / eMMC
- SDHCI controller at 0xFE340000 (Arasan), IRQ 158
- blk_emmc.c: read-only PIO mode, single-block CMD17 reads
- MBR layout: P1=FAT32 boot (type 0x0b), P2=ext2 system (type 0x83)
- blk_emmc.c scans MBR for type 0x83, adds partition LBA offset to all reads
- ext2 superblock verified (magic 0xEF53) during init

### SMP
- KernelMaxNumNodes MUST be 1 (single core)
- Setting to 4 causes silent boot failure (secondary cores not properly parked)
- Requires elfloader secondary core wakeup support before re-enabling

### Disabled Drivers (v0.4.98)
- GENET Ethernet (src/plat/rpi4/net_genet.c): crashes during 64KB MMIO mapping
  plat_net_init() returns -1 immediately. 731 lines of driver code from rpi4
  branch needs debugging (sel4platsupport_alloc_frame_at fails or hangs).
- VideoCore Display (src/plat/rpi4/display_vc.c): VKA allocator assertion
  in _utspace_split_alloc when trying to map VC mailbox at 0xFE00B880.
  Phase A fails (no diagnostic stub -> fb_info at 0x3A000000 is garbage).
  Phase B crashes in allocator. plat_display_init() returns -1 immediately.

### Memory
- RPi4 morecore: 8MB (settings-rpi4.cmake, separate from QEMU 6MB)
- gpu_mem=64 in config.txt (minimum for firmware operation)
- total_mem=4096
- RAM display bug: shows -217 MB (unsigned overflow in DTB memory parsing)

## config.txt (working, saved as hw/rpi4/config.txt)

    arm_64bit=1
    kernel=kernel8.img
    enable_uart=1
    uart_2ndstage=1
    core_freq=250
    core_freq_min=250
    gpu_mem=64
    total_mem=4096

No kernel_address (firmware ignores it for ARM64 images).
No dtoverlay=disable-bt (need mini UART on GPIO 14/15).

## Complete Build + Flash Procedure

### 1. Build RPi4 target

    cd ~/Desktop/github_repos/AIOS
    cd build-rpi4 && ninja && cd ..

### 2. Generate relocator + kernel8.img

    # Extract entry point
    ENTRY=$(aarch64-linux-gnu-readelf -h build-rpi4/images/aios_root-image-arm-bcm2711 \
        | grep "Entry point" | awk -F'0x' '{print $2}')

    # Create flat binary
    aarch64-linux-gnu-objcopy -O binary \
        build-rpi4/images/aios_root-image-arm-bcm2711 /tmp/kernel8_raw.img

    # Generate relocator.S with correct entry address (use Python -- see scripts)
    # Assemble: aarch64-linux-gnu-as -o /tmp/relocator.o /tmp/relocator.S
    # Link: aarch64-linux-gnu-ld -Ttext=0 -o /tmp/relocator.elf /tmp/relocator.o
    # Binary: aarch64-linux-gnu-objcopy -O binary /tmp/relocator.elf /tmp/relocator.bin
    # Pad stub to 4096 bytes, concatenate: cat stub + payload > kernel8.img

### 3. Prepare SD card (Method 2 -- macOS diskutil)

    # Partition: FAT32 boot + free space for ext2
    diskutil partitionDisk /dev/diskN MBR FAT32 AIOSBOOT 64M "Free Space" SYS 0

    # Copy boot files
    cp hw/rpi4/firmware/start4.elf  /Volumes/AIOSBOOT/start4.elf
    cp hw/rpi4/firmware/fixup4.dat  /Volumes/AIOSBOOT/fixup4.dat
    cp hw/rpi4/firmware/start4.elf  /Volumes/AIOSBOOT/start_cd.elf
    cp hw/rpi4/firmware/fixup4.dat  /Volumes/AIOSBOOT/fixup_cd.dat
    cp hw/rpi4/firmware/bcm2711-rpi-4-b.dtb /Volumes/AIOSBOOT/
    cp hw/rpi4/config.txt           /Volumes/AIOSBOOT/
    cp /tmp/kernel8.img             /Volumes/AIOSBOOT/

    # Unmount and patch MBR for ext2 partition
    diskutil unmountDisk /dev/diskN
    # Read MBR sector, add P2 entry (type=0x83, LBA=131072, 262144 sectors)
    # Write patched MBR back (must be 512-byte aligned write)
    # dd ext2 image: sudo dd if=ext2.img of=/dev/rdiskN bs=1m seek=64

### 4. Connect serial

    screen /dev/tty.usbserial-* 115200
    # Exit screen: Ctrl-A, k, y

## Firmware Files (hw/rpi4/firmware/)

- start4.elf: RPi4 GPU firmware (2.3MB) -- also copied as start_cd.elf
- fixup4.dat: Memory split config (5.5KB) -- also copied as fixup_cd.dat
- bcm2711-rpi-4-b.dtb: Device tree blob (56KB)

## Debugging

- Green ACT LED: ON during firmware, OFF after handoff (normal), OFF immediately = crash
- uart_2ndstage=1: enables firmware MESS:00: debug messages on serial
- HDMI EDID errors: normal when no monitor connected (harmless)
- Elfloader is SILENT: printf disabled, no output until root task UART init
- If boot hangs after firmware: check relocator, check KernelMaxNumNodes=1
- "No compatible kernel found": missing ARM64 header or corrupt FAT32
- "Kernel relocated to 0x80000": normal (relocator handles this)

## Known Bugs (v0.4.98)
- RAM display: -217 MB (unsigned overflow in DTB memory parsing)
- Banner: shows v0.4.96/build 1528 (stale ext2 binaries, need disk rebuild)
- Arch string: says Cortex-A53 and virtio-blk (hardcoded, not runtime)

# AIOS Raspberry Pi Port Design

## Executive Summary

AIOS currently targets QEMU `virt` (AArch64) with virtio device drivers. This
document designs a **multi-target build** that keeps the QEMU target intact and
adds support for Raspberry Pi hardware, starting with **RPi4** (BCM2711) and
later **RPi5** (BCM2712).

The key insight is that AIOS already has partial hardware abstraction:

- DTB-based discovery (`hw_info`) for MMIO addresses and IRQs
- Function-pointer block I/O (`blk_read_fn` / `blk_write_fn` in ext2)
- Architecture isolation layer (`src/arch/`)
- Lock-free ring buffer between net_driver and net_server

The port introduces a **Platform Abstraction Layer (PAL)** that formalizes these
patterns into compile-time selectable driver sets. The entire OS above the PAL
(VFS, ext2, pipes, processes, POSIX shim, SSH, shells) is unchanged.

## Why: Multi-Target Instead of Replacement

| Principle | Rationale |
|-----------|-----------|
| Keep QEMU target | Fast development cycle (no SD card flash, instant boot) |
| Share all core code | One codebase, no divergence, bug fixes apply everywhere |
| Compile-time selection | No runtime dispatch overhead, no dead driver code in binary |
| Incremental porting | Bring up serial first, then storage, then network |

## seL4 Platform Status

| Board | SoC | seL4 BSP | Status |
|-------|-----|----------|--------|
| QEMU virt | -- | `qemu-arm-virt` | Working (current target) |
| RPi 3B/3B+ | BCM2837 | `bcm2837` | Upstream, verified |
| RPi 4B | BCM2711 | `bcm2711` / `rpi4` | Upstream, verified |
| RPi 5 | BCM2712 | -- | **No BSP exists** |

**Strategy**: Target RPi4 first (seL4 BSP exists). The RPi5 requires a new
seL4 BSP for BCM2712, which can be developed incrementally since the core
peripherals (GIC-400, ARM generic timer, PL011 UART) are the same IP blocks
already supported.

### RPi5 / RP1 Challenge

The RPi5 moves Ethernet, USB, SD card, and GPIO behind the **RP1 south bridge**,
accessed via PCIe x4. This means I/O drivers require a PCIe root complex driver
before any device behind RP1 can be reached. No BCM2712 datasheet has been
published. The PL011 UART on the BCM2712 itself is directly accessible (no RP1
needed), so serial console works without PCIe.

## Repository Layout

```
src/
  plat/
    plat.h                  PAL dispatcher (like arch.h)
    blk_hal.h               Block device interface
    net_hal.h               Network device interface
    display_hal.h           Display device interface

    qemu-virt/
      blk_virtio.c          Current blk_io.c (virtio-blk)
      blk_virtio_init.c     Current boot_fs_init.c virtio probe
      net_virtio.c           Current net_driver.c (virtio-net)
      net_virtio_init.c      Current boot_net_init.c
      display_ramfb.c        Current boot_display_init.c (fw_cfg + ramfb)

    rpi4/
      blk_emmc.c            BCM2835 SDHCI block read/write
      blk_emmc_init.c        eMMC controller init from DTB
      net_genet.c            BCM54213 GENET Ethernet driver
      net_genet_init.c       GENET init from DTB
      display_vc_mailbox.c   VideoCore mailbox framebuffer (optional)

    rpi5/                    (future -- requires seL4 BCM2712 BSP)
      blk_emmc.c             Reuse rpi4 SDHCI (same controller)
      net_rp1_genet.c        GENET behind RP1/PCIe
      pcie_bcm2712.c         PCIe root complex driver

  boot/
    boot_dtb.c              Unchanged (generic DTB parser)
    boot_fs_init.c          Calls plat_blk_init() instead of virtio probe
    boot_net_init.c          Calls plat_net_init() instead of virtio probe
    boot_display_init.c      Calls plat_display_init()
    boot_log_init.c          Calls plat_blk_init_log() for second drive
    boot_services.c          Unchanged
    blk_io.c                 Deleted (absorbed into plat/*/blk_*.c)

  arch/                     Unchanged (aarch64 barriers, page ops)
  servers/
    net_driver.c             Calls plat_net_rx_poll() instead of virtio ring
  ...                       Everything else unchanged
```

## Platform Abstraction Layer (PAL)

### plat.h -- Dispatcher

```c
#ifndef AIOS_PLAT_H
#define AIOS_PLAT_H

#if defined(PLAT_QEMU_VIRT)
  #include "qemu-virt/plat_qemu.h"
#elif defined(PLAT_RPI4)
  #include "rpi4/plat_rpi4.h"
#elif defined(PLAT_RPI5)
  #include "rpi5/plat_rpi5.h"
#else
  #error "No platform selected. Define PLAT_QEMU_VIRT, PLAT_RPI4, or PLAT_RPI5."
#endif

#endif
```

### blk_hal.h -- Block Device Interface

```c
#ifndef AIOS_BLK_HAL_H
#define AIOS_BLK_HAL_H

#include <stdint.h>

/* Block device context (opaque per-platform) */
typedef struct blk_dev blk_dev_t;

/* Initialize primary block device from DTB.
 * Returns 0 on success, populates *dev.
 * read_fn/write_fn are set for ext2_init(). */
int plat_blk_init(blk_dev_t *dev);

/* Initialize secondary (log) block device.
 * Returns 0 on success, -1 if no second device. */
int plat_blk_init_log(blk_dev_t *dev);

/* Sector I/O -- matches ext2 blk_read_fn / blk_write_fn signatures */
int plat_blk_read(uint64_t sector, void *buf);
int plat_blk_write(uint64_t sector, const void *buf);
int plat_blk_read_log(uint64_t sector, void *buf);
int plat_blk_write_log(uint64_t sector, const void *buf);

#endif
```

### net_hal.h -- Network Device Interface

```c
#ifndef AIOS_NET_HAL_H
#define AIOS_NET_HAL_H

#include <stdint.h>

/* Initialize network hardware from DTB.
 * Sets up DMA, IRQ, MAC address.
 * Returns 0 on success. */
int plat_net_init(void);

/* Transmit an Ethernet frame (no virtio header).
 * Returns 0 on success, -1 on error. */
int plat_net_tx(const uint8_t *frame, uint32_t len);

/* Driver thread main loop.
 * Waits on hardware IRQ, drains RX into net_rx_ring,
 * signals net_server notification. */
void plat_net_driver_fn(void *arg0, void *arg1, void *ipc_buf);

/* Get MAC address (6 bytes). */
void plat_net_get_mac(uint8_t mac[6]);

#endif
```

### display_hal.h -- Display Device Interface

```c
#ifndef AIOS_DISPLAY_HAL_H
#define AIOS_DISPLAY_HAL_H

#include <stdint.h>

/* Initialize framebuffer hardware.
 * Returns 0 on success, -1 if no display (serial-only). */
int plat_display_init(uint32_t width, uint32_t height);

/* Get framebuffer base address and stride.
 * Returns NULL if display not initialized. */
uint32_t *plat_display_get_fb(uint32_t *stride);

#endif
```

## How Current Code Maps to PAL

The existing code already uses patterns that map cleanly:

| Current Code | PAL Function | Notes |
|-------------|-------------|-------|
| `blk_read_sector()` in blk_io.c | `plat_blk_read()` | Same signature |
| `blk_write_sector()` in blk_io.c | `plat_blk_write()` | Same signature |
| `blk_read_sector_log()` in blk_io.c | `plat_blk_read_log()` | Same signature |
| `blk_write_sector_log()` in blk_io.c | `plat_blk_write_log()` | Same signature |
| `net_tx_send()` in net_stack.c | `plat_net_tx()` | Strip virtio header handling |
| `net_driver_fn()` in net_driver.c | `plat_net_driver_fn()` | Same rx_ring output |
| Virtio probe in boot_fs_init.c | `plat_blk_init()` | Encapsulate probe + DMA |
| Virtio probe in boot_net_init.c | `plat_net_init()` | Encapsulate probe + DMA |
| ramfb setup in boot_display_init.c | `plat_display_init()` | Encapsulate FB alloc |

### ext2 integration (unchanged)

ext2 already uses function pointers. The only change is which function is passed:

```c
/* Before (direct call) */
ext2_init(&ext2, blk_read_sector, 0);
ext2_init_write(&ext2, blk_write_sector);

/* After (PAL call -- same signature) */
ext2_init(&ext2, plat_blk_read, 0);
ext2_init_write(&ext2, plat_blk_write);
```

### net_server integration (unchanged)

The net_server reads from `net_rx_ring` (lock-free SPSC ring) and is completely
driver-agnostic. The PAL driver thread writes to `net_rx_ring` exactly as
`net_driver_fn` does today. `net_tx_send()` calls `plat_net_tx()` instead of
directly manipulating virtio TX descriptors.

### UART (no PAL needed)

PL011 UART is used on both QEMU virt and RPi4. The DTB parser already discovers
the UART base address and IRQ number. The UART code in `aios_root.c` uses
`hw_info.uart_paddr` and `hw_info.uart_irq`. No PAL wrapper needed -- PL011
register layout is identical across platforms.

## Build System Changes

### settings.cmake

```cmake
# Platform selection -- one of: qemu-arm-virt, rpi4
# RPi5 future: bcm2712 (requires seL4 BSP)
set(KernelPlatform "qemu-arm-virt" CACHE STRING "" FORCE)

if(KernelPlatform STREQUAL "qemu-arm-virt")
    set(AIOS_PLATFORM "PLAT_QEMU_VIRT")
    set(KernelArmCPU "cortex-a53" CACHE STRING "" FORCE)
    set(KernelArmHypervisorSupport ON CACHE BOOL "" FORCE)
elseif(KernelPlatform STREQUAL "rpi4")
    set(AIOS_PLATFORM "PLAT_RPI4")
    set(KernelArmCPU "cortex-a72" CACHE STRING "" FORCE)
    set(RPI4_MEMORY "4096" CACHE STRING "" FORCE)
    set(KernelArmHypervisorSupport OFF CACHE BOOL "" FORCE)
endif()
```

### CMakeLists.txt (platform source selection)

```cmake
# Platform-specific driver sources
if(AIOS_PLATFORM STREQUAL "PLAT_QEMU_VIRT")
    set(PLAT_SOURCES
        src/plat/qemu-virt/blk_virtio.c
        src/plat/qemu-virt/blk_virtio_init.c
        src/plat/qemu-virt/net_virtio.c
        src/plat/qemu-virt/net_virtio_init.c
        src/plat/qemu-virt/display_ramfb.c
    )
elseif(AIOS_PLATFORM STREQUAL "PLAT_RPI4")
    set(PLAT_SOURCES
        src/plat/rpi4/blk_emmc.c
        src/plat/rpi4/blk_emmc_init.c
        src/plat/rpi4/net_genet.c
        src/plat/rpi4/net_genet_init.c
        src/plat/rpi4/display_vc_mailbox.c
    )
endif()

target_compile_definitions(aios_root PRIVATE ${AIOS_PLATFORM})
target_sources(aios_root PRIVATE ${PLAT_SOURCES})
```

### Separate build directories

```
build-04/          QEMU virt (existing, unchanged)
build-rpi4/        RPi4 build
```

Build command for RPi4:

```
mkdir build-rpi4 && cd build-rpi4
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- \
    -DKernelPlatform=rpi4 ..
ninja
```

## hw_info.h Additions

```c
typedef struct {
    /* UART -- existing fields, no changes */
    uint64_t uart_paddr;
    uint32_t uart_irq;
    int      has_uart;

    /* Virtio MMIO -- QEMU only */
    uint64_t virtio_base;
    uint32_t virtio_size;
    int      virtio_count;
    int      has_virtio;

    /* fw_cfg -- QEMU only */
    uint64_t fwcfg_paddr;
    int      has_fwcfg;

    /* eMMC/SDHCI -- RPi */
    uint64_t emmc_paddr;       /* BCM2835 SDHCI base */
    uint32_t emmc_irq;
    int      has_emmc;

    /* GENET Ethernet -- RPi4 */
    uint64_t genet_paddr;      /* BCM54213 GENET base */
    uint32_t genet_irq;
    int      has_genet;

    /* VideoCore mailbox -- RPi */
    uint64_t vc_mbox_paddr;    /* ARM-to-VC mailbox base */
    int      has_vc_mbox;

    /* CPU, Memory, DTB -- unchanged */
    int      cpu_count;
    char     cpu_compat[32];
    uint64_t mem_base;
    uint64_t mem_size;
    int      dtb_valid;
} aios_hw_info_t;
```

### DTB Parser Additions (boot_dtb.c)

New parse functions for RPi hardware:

```
parse_emmc()    -- find "brcm,bcm2835-sdhci" or "brcm,bcm2711-emmc2"
parse_genet()   -- find "brcm,bcm2711-genet-v5"
parse_vc_mbox() -- find "brcm,bcm2835-mbox"
```

These are called unconditionally (like existing parse_virtio) and populate
`hw_info.has_emmc`, etc. On QEMU, they find nothing and fields stay zero.
On RPi4, they discover real hardware. Platform drivers check `has_*` flags.

## RPi4 Driver Details

### Block: BCM2835 SDHCI (SD Card)

The BCM2835/BCM2711 SDHCI controller is well-documented:

- Memory-mapped registers at DTB-discovered address
- Standard SD Host Controller Interface (SDIO 3.0)
- 4-bit data bus, up to 50MHz clock
- CMD17 (READ_SINGLE_BLOCK) / CMD24 (WRITE_SINGLE_BLOCK) for sector I/O
- No DMA needed for initial implementation (PIO mode via data register)

Implementation approach:
1. PIO mode first (simplest, correct, matches AIOS philosophy)
2. DMA mode later for performance (ADMA2 descriptor table)

Reference: Linux `drivers/mmc/host/bcm2835-mmc.c`, Circle `lib/bcm2835-sdhci.cpp`

The SDHCI driver provides `plat_blk_read()` / `plat_blk_write()` with the same
512-byte sector interface that ext2 expects.

### Network: BCM54213 GENET v5

The RPi4 Gigabit Ethernet controller:

- Memory-mapped GENET MAC + MDIO for BCM54213 PHY
- Standard descriptor ring (DMA ring, similar to virtio conceptually)
- 256 TX + 256 RX descriptors
- Interrupt on RX completion
- MAC address from DTB `local-mac-address` property

Implementation approach:
1. Single-speed (100Mbps) first, autoneg later
2. RX ring drains into existing `net_rx_ring` (same SPSC pattern)
3. TX via `plat_net_tx()` writes frame to TX descriptor ring

Reference: Linux `drivers/net/ethernet/broadcom/genet/`, Circle `lib/bcmgenet.cpp`

### Display: VideoCore Mailbox Framebuffer

The RPi firmware provides a framebuffer via the ARM-to-VideoCore mailbox:

- Send "allocate framebuffer" message via mailbox registers
- Firmware returns physical address and pitch of linear framebuffer
- Same XRGB8888 format as current ramfb

Implementation approach:
1. Request 1024x768x32 framebuffer via mailbox property interface
2. Map returned physical address into root task VSpace
3. Provide to display subsystem via `plat_display_get_fb()`

Reference: RPi firmware wiki, Circle `lib/bcmmailbox.cpp`

### Serial: PL011 UART (no driver changes)

RPi4 PL011 UART at GPIO pins 14/15:
- Same register layout as QEMU PL011
- Different base address (0xfe201000 vs 0x9000000) -- DTB handles this
- Different IRQ number -- DTB handles this
- Baud rate set by RPi firmware before seL4 boots

## Globals Refactoring

Current virtio globals in `root_shared.h`:

```c
extern volatile uint32_t *blk_vio;
extern uint8_t *blk_dma;
extern uint64_t blk_dma_pa;
extern volatile uint32_t *blk_vio_log;
extern uint8_t *blk_dma_log;
extern uint64_t blk_dma_pa_log;
```

These move into the platform driver files:

- `blk_vio`, `blk_dma`, `blk_dma_pa` -> `src/plat/qemu-virt/blk_virtio.c` (static)
- `blk_vio_log`, `blk_dma_log`, `blk_dma_pa_log` -> same file (static)
- `net_vio`, `net_dma`, `net_dma_pa` -> `src/plat/qemu-virt/net_virtio.c` (static)

Shared globals that remain in `root_shared.h`:
- `net_rx_ring` -- driver-agnostic, shared between net_driver and net_server
- `net_srv_ntfn_cap` -- seL4 notification for server wakeup
- `net_irq_handler_cap` -- seL4 IRQ handler (allocated by platform init)

## Boot Flow Comparison

```
QEMU virt                          RPi4
---------                          ----
QEMU loads ELF-loader              RPi firmware loads from SD card
  |                                  |
seL4 kernel starts                 seL4 kernel starts
  |                                  |
Root task init                     Root task init
  |                                  |
boot_dtb_init()                    boot_dtb_init()
  discovers: PL011, virtio           discovers: PL011, eMMC, GENET, VC mbox
  |                                  |
boot_fs_init()                     boot_fs_init()
  plat_blk_init()                    plat_blk_init()
    virtio-blk probe + DMA             SDHCI init + CMD0/CMD8/ACMD41
  ext2_init(plat_blk_read)          ext2_init(plat_blk_read)
  |                                  |
boot_log_init()                    boot_log_init()
  plat_blk_init_log()                plat_blk_init_log()
    second virtio-blk                  (single SD card -- no log drive)
  |                                  |
boot_display_init()                boot_display_init()
  plat_display_init()                plat_display_init()
    fw_cfg + ramfb                     VC mailbox framebuffer
  |                                  |
boot_net_init()                    boot_net_init()
  plat_net_init()                    plat_net_init()
    virtio-net probe + DMA             GENET + PHY init
  |                                  |
[rest of boot identical]           [rest of boot identical]
  pipe_server, fs_thread,            pipe_server, fs_thread,
  exec_thread, getty, dash           exec_thread, getty, dash
```

## RPi4 Boot Media

### Deploying to SD Card

The RPi4 firmware loads the seL4 ELF-loader from the SD card boot partition
(FAT32). The ext2 system disk is a separate partition on the same card.

```
SD Card Layout:
  Partition 1: FAT32 (boot)
    - bootcode.bin, start4.elf, fixup4.dat  (RPi firmware)
    - config.txt                             (kernel=aios.img, arm_64bit=1)
    - aios.img                               (seL4 ELF-loader image)
  Partition 2: ext2 (system)
    - Same content as current disk_ext2.img
    - /bin/, /etc/, /root/, /tmp/
```

### config.txt

```
arm_64bit=1
kernel=aios.img
enable_uart=1
uart_2ndstage=1
dtoverlay=disable-bt
core_freq=250
core_freq_min=250
```

### mksdcard.py (new script)

```
scripts/mksdcard.py /dev/diskN
  1. Partition SD card (FAT32 + ext2)
  2. Copy RPi firmware + config.txt + aios.img to FAT32
  3. Write ext2 system image to partition 2
```

## Implementation Phases

### Phase 0: PAL Refactor (QEMU only, no new hardware)

Move existing virtio code behind PAL interfaces. QEMU target must still build
and boot identically after this phase. This is pure refactoring.

1. Create `src/plat/` directory structure
2. Create `plat.h`, `blk_hal.h`, `net_hal.h`, `display_hal.h`
3. Move `blk_io.c` -> `src/plat/qemu-virt/blk_virtio.c`
4. Move virtio probe from `boot_fs_init.c` -> `src/plat/qemu-virt/blk_virtio_init.c`
5. Move `net_driver.c` -> `src/plat/qemu-virt/net_virtio.c`
6. Move virtio probe from `boot_net_init.c` -> `src/plat/qemu-virt/net_virtio_init.c`
7. Move ramfb code from `boot_display_init.c` -> `src/plat/qemu-virt/display_ramfb.c`
8. Update `boot_fs_init.c` to call `plat_blk_init()` + `plat_blk_read()`
9. Update `boot_net_init.c` to call `plat_net_init()`
10. Update `net_stack.c` `net_tx_send()` to call `plat_net_tx()`
11. Move virtio globals from `root_shared.h` to platform files
12. Update CMakeLists.txt for platform source selection
13. Verify QEMU boots and all tests pass

**Test**: Full QEMU boot, SSH, Ctrl-C, pipe tests -- identical behavior.

### Phase 1: RPi4 Serial Boot

Minimal RPi4 target: serial console only, no storage or network.

1. Create `settings-rpi4.cmake` (KernelPlatform=rpi4, cortex-a72)
2. Add stub `plat_blk_init()` that returns -1 (no storage)
3. Add stub `plat_net_init()` that returns -1 (no network)
4. Add stub `plat_display_init()` that returns -1 (no display)
5. Build with `cmake -DKernelPlatform=rpi4`
6. Deploy to RPi4 SD card (FAT32 boot partition)
7. Verify serial output: AIOS banner, DTB report

**Test**: See `[boot]` messages on RPi4 UART serial console.

### Phase 2: RPi4 SD Card (ext2 from disk)

Add SDHCI block driver. This enables the full filesystem.

1. Implement `blk_emmc_init.c` (SDHCI controller init, CMD0/CMD8/ACMD41/CMD2/CMD3)
2. Implement `blk_emmc.c` (CMD17 read, CMD24 write, PIO mode)
3. Add DTB parser for "brcm,bcm2835-sdhci"
4. Hook into ext2 via `plat_blk_read()` / `plat_blk_write()`
5. Partition SD card (FAT32 boot + ext2 system)
6. Boot: serial + filesystem + login + dash

**Test**: Login prompt on RPi4, `ls /`, `cat /etc/passwd`, shell commands.

### Phase 3: RPi4 Network

Add GENET Ethernet driver.

1. Implement `net_genet_init.c` (GENET MAC init, MDIO PHY reset, DMA ring setup)
2. Implement `net_genet.c` (RX poll -> net_rx_ring, TX from descriptor ring)
3. Add DTB parser for "brcm,bcm2711-genet-v5"
4. Wire into net_server (same rx_ring interface)

**Test**: `ping` from host, SSH into RPi4, sshd session.

### Phase 4: RPi4 Display (Optional)

Add VideoCore mailbox framebuffer.

1. Implement `display_vc_mailbox.c` (mailbox property interface, FB allocate)
2. Add DTB parser for "brcm,bcm2835-mbox"
3. Map framebuffer, provide to display subsystem

**Test**: AIOS splash screen on HDMI, text rendering.

### Phase 5: RPi5 (Future)

Requires new seL4 BSP for BCM2712. Steps:

1. Create seL4 `src/plat/bcm2712/` (kernel BSP: GIC, timer, UART, memory map)
2. SDHCI driver likely reusable from RPi4 (same controller IP)
3. PCIe root complex driver for RP1 access
4. GENET Ethernet driver via RP1 (same MAC, different bus)

## What Does NOT Change

The following code is completely platform-agnostic and requires zero
modification for the RPi port:

```
src/ext2.c                  ext2 filesystem (uses blk_read_fn pointers)
src/vfs.c                   VFS layer
src/procfs.c                /proc filesystem
src/aios_auth.c             Authentication
src/aios_log.c              Logging
src/servers/pipe_server.c   Pipes, fork, exec, wait, signals
src/servers/fs_server.c     File operations
src/servers/exec_server.c   ELF loading
src/servers/thread_server.c Thread management
src/servers/net_server.c    TCP/IP stack (reads from net_rx_ring)
src/net/net_stack.c         Protocol stack (ARP, IP, TCP, UDP)
src/net/net_tcp.c           TCP state machine
src/process/fork.c          Fork implementation
src/process/reap.c          Process cleanup
src/lib/aios_posix.c        POSIX shim
src/lib/posix_*.c           All POSIX syscall implementations
src/lib/posix_net.c         Socket API
src/arch/                   Architecture layer (aarch64 unchanged)
src/boot/boot_dtb.c         DTB parser (gains new parse functions)
src/boot/boot_services.c    Server thread spawning
src/apps/                   All user programs
src/ssh/                    SSH server
include/aios/ext2.h         Already uses function pointers
include/aios/net.h          Protocol headers (platform agnostic)
```

**Estimated unchanged**: ~95% of codebase.

## Hardware Reference Sources

| Component | Reference |
|-----------|-----------|
| BCM2835 SDHCI | Linux `drivers/mmc/host/bcm2835-mmc.c`, BCM2835 ARM Peripherals datasheet Ch. 5 |
| BCM2711 GENET | Linux `drivers/net/ethernet/broadcom/genet/`, QNX RPi5 BSP |
| VideoCore mailbox | RPi firmware wiki, Linux `drivers/firmware/raspberrypi.c` |
| RPi4 DTB | Linux `arch/arm64/boot/dts/broadcom/bcm2711-rpi-4-b.dts` |
| RPi5 DTB | Linux `arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dts` |
| seL4 RPi4 BSP | `seL4/src/plat/bcm2711/`, `seL4_tools/cmake-tool/helpers/application_settings.cmake` |
| RP1 south bridge | Linux `drivers/pci/controller/pcie-brcmstb.c`, RP1 datasheet (partial) |
| Circle bare-metal | `github.com/rsta2/circle` (RPi4 SDHCI, GENET reference implementations) |


## Appendix: QEMU HVF Acceleration (Apple Silicon)

### Findings (tested on M3 Max, QEMU 10.2.2)

| Config | Result |
|--------|--------|
| `-cpu max -smp 4` | QEMU exit 1 (HVF cannot emulate named CPUs) |
| `-cpu cortex-a72 -smp 4` | QEMU exit 1 (same reason) |
| `-cpu host -smp 4` | QEMU exit 1 (SMP bring-up failure) |
| `-cpu host -smp 1` | ELF-loader runs, seL4 kernel hangs at init |

### Root Cause

seL4 is built with `KernelArmHypervisorSupport ON`, which expects to run at
EL2 and self-manage the hypervisor. Under HVF, macOS owns EL2 and presents
the guest at EL1. The kernel hangs during early EL2-to-EL1 transition because
the expected privilege level is not available.

Additionally, HVF only supports `-cpu host` (exposes real Apple Silicon cores),
not named ARM CPU models like cortex-a53/a72. SMP under HVF requires PSCI
emulation that may not match seL4 expectations.

### What Would Be Needed

A separate build target (`settings-hvf.cmake`) with:

- KernelPlatform = qemu-arm-virt
- KernelSel4Arch = aarch64
- KernelArmHypervisorSupport = OFF
- KernelMaxNumNodes = 1

This is a lower priority than the RPi4 port. TCG emulation works correctly
today and is the primary development environment.

#
# AIOS 0.4.x build settings -- Raspberry Pi 4 (BCM2711)
#
cmake_minimum_required(VERSION 3.16.0)

set(project_dir "${CMAKE_CURRENT_LIST_DIR}")
set(SEL4_CONFIG_DEFAULT_ADVANCED ON)

# Platform: BCM2711 (RPi4, Cortex-A72)
set(KernelPlatform "bcm2711" CACHE STRING "" FORCE)
set(KernelSel4Arch "aarch64" CACHE STRING "" FORCE)
set(KernelArmCPU "cortex-a72" CACHE STRING "" FORCE)
set(AIOS_PLATFORM "PLAT_RPI4" CACHE STRING "" FORCE)

# RPi4 memory (1024, 2048, 4096, 8192)
set(RPI4_MEMORY "4096" CACHE STRING "" FORCE)

# SMP: RPi4 has 4 Cortex-A72 cores -- all 4 boot. HW-VERIFIED v0.4.179
# (2026-06-07): the elfloader spin-table brings up the 3 secondaries
# ("Boot cpu id = 0x0" -> "Core 1/2/3 is up"), the kernel bootstraps 4-core SMP,
# AIOS boots and /proc reports cores=4 (ping 192.168.0.8 0% loss). The
# v0.4.134/178 "hang at smp_boot.c:119" was a GHOST: the elfloader was SILENT
# (its aux-uart driver skipped console registration -> every printf no-op'd) so
# the bring-up was invisible, and the v0.4.178 dtoverlay=disable-bt "make it
# visible on PL011" attempt only DISCONNECTED the mini-UART console from the
# cable AND broke the kernel boot (root task drives the mini-UART). Real fix:
# register the elfloader mini-UART console (deps bcm-uart.c uart_set_out, the
# putchar is bounded) and keep the known-good mini-UART config (no disable-bt,
# core_freq=250). See the project_rpi4_smp memory + hw/rpi4/BOOT_NOTES.md.
# v0.4.220: single-core. The BCM2711 whole-system 32.4s stalls (TLBI+DSB) have
# TWO triggers: WFI retention (fixed by the no-WFI idle) and an SMP-build
# broadcast-TLBI path (unfixed). All AIOS threads and children are pinned to
# core 0 anyway, so SMP=4 currently buys nothing. Restore to 4 only with the
# stall A/B harness in hand (see project_stall_hunt memory).
set(KernelMaxNumNodes 1 CACHE STRING "" FORCE)

# Debug
set(KernelVerificationBuild OFF CACHE BOOL "" FORCE)
set(KernelDebugBuild ON CACHE BOOL "" FORCE)
set(KernelPrinting ON CACHE BOOL "" FORCE)

# MCS scheduler
set(KernelIsMCS OFF CACHE BOOL "" FORCE)

# GCC 15 workaround
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=default" CACHE STRING "" FORCE)

# RPi4: no hypervisor support (bare metal EL1)
set(KernelArmHypervisorSupport OFF CACHE BOOL "" FORCE)
set(KernelRootCNodeSizeBits 16 CACHE STRING "" FORCE)
set(KernelArmExportPCNTUser ON CACHE BOOL "" FORCE)

# Heap: 8MB morecore
set(LibSel4MuslcSysMorecoreBytes 8388608 CACHE STRING "" FORCE)

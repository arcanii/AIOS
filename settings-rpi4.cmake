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

# SMP: RPi4 has 4 Cortex-A72 cores. v0.4.134 enabled all 4, but the first
# real-hardware boot (v0.4.134, build 1691) hung at the firmware-to-kernel
# handoff: kernel8.img loads and relocates to 0x80000, then the boot goes
# silent and never reaches "Bootstrapping kernel". The only stage that runs
# there and differs from the known-good v0.4.98 (single-core) build is the
# elfloader spin-table secondary-core bring-up
# (deps/seL4_tools/elfloader-tool/src/arch-arm/drivers/smp-spin-table.c).
# The elfloader is silent on this UART, so the hang is invisible but parked
# in that bring-up loop. v0.4.135 falls back to single-core so the full
# v0.4.99-134 feature set boots on hardware. Re-enable (set to 4) once the
# spin-table hang is understood -- see hw/rpi4/HARDWARE_TEST.md and the
# per-core boot-trace plan (item E) in docs/SEL4_DEVInvestigation.md.
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

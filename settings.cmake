#
# AIOS 0.4.x build settings
#
cmake_minimum_required(VERSION 3.16.0)

set(project_dir "${CMAKE_CURRENT_LIST_DIR}")
set(SEL4_CONFIG_DEFAULT_ADVANCED ON)

# Platform: QEMU virt (default)
set(KernelPlatform "qemu-arm-virt" CACHE STRING "" FORCE)
set(KernelSel4Arch "aarch64" CACHE STRING "" FORCE)
set(KernelArmCPU "cortex-a53" CACHE STRING "" FORCE)
set(AIOS_PLATFORM "PLAT_QEMU_VIRT" CACHE STRING "" FORCE)

# SMP
set(KernelMaxNumNodes 4 CACHE STRING "" FORCE)

# Debug
set(KernelVerificationBuild OFF CACHE BOOL "" FORCE)
set(KernelDebugBuild ON CACHE BOOL "" FORCE)
set(KernelPrinting ON CACHE BOOL "" FORCE)

# MCS scheduler
set(KernelIsMCS OFF CACHE BOOL "" FORCE)

# GCC 15 workaround: musl weak_alias generates protected visibility symbols
# which the linker cannot copy-relocate. Force default visibility.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=default" CACHE STRING "" FORCE)
set(KernelArmHypervisorSupport ON CACHE BOOL "" FORCE)
set(KernelRootCNodeSizeBits 16 CACHE STRING "" FORCE)
set(KernelArmExportPCNTUser ON CACHE BOOL "" FORCE)

# Heap: 12MB morecore for tcc self-hosting and large programs
set(LibSel4MuslcSysMorecoreBytes 12582912 CACHE STRING "" FORCE)

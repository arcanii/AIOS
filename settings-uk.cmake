#
# settings-uk.cmake -- build settings for the AIOS USERSPACE-KERNEL seL4 port (the real pal_sel4).
#
# Cloned from the 0.4.x settings.cmake (the proven qemu-arm-virt recipe) and tightened to the
# ratified target (docs/PLAN_20260709_sel4_real_port.md): AArch64 / seL4 15.0.0 / **non-MCS** /
# **UNICORE** -- matching the shape of the verified AArch64 configuration (which is unicore + non-MCS;
# MCS FC on AArch64 is ~Q3/27). Development is on qemu-arm-virt with a DEBUG (printing) kernel so the
# root task's console output goes to serial via seL4_DebugPutChar during bring-up (a real serial /
# console server comes in a later phase). Select this workspace with -DAIOS_UK_BUILD=1 (which makes
# projects/aios-uk the rootserver and skips the 0.4.x projects/aios).
#
cmake_minimum_required(VERSION 3.16.0)

set(project_dir "${CMAKE_CURRENT_LIST_DIR}")
set(SEL4_CONFIG_DEFAULT_ADVANCED ON)

# Platform: QEMU virt / aarch64 / cortex-a53 (the dev target; a real board is a later phase).
set(KernelPlatform "qemu-arm-virt" CACHE STRING "" FORCE)
set(KernelSel4Arch "aarch64" CACHE STRING "" FORCE)
set(KernelArmCPU "cortex-a53" CACHE STRING "" FORCE)
set(AIOS_PLATFORM "PLAT_QEMU_VIRT" CACHE STRING "" FORCE)

# UNICORE -- the verified AArch64 configs pin KernelMaxNumNodes=1; the uk kernel is single-threaded by
# design, and unicore sidesteps the multi-core teardown machinery entirely (docs/PLAN §2).
set(KernelMaxNumNodes 1 CACHE STRING "" FORCE)

# Dev kernel: debug + printing ON (so seL4_DebugPutChar -> serial works as the bring-up console),
# verification build OFF (that is a later, pinned, printing-OFF variant -- Phase I).
set(KernelVerificationBuild OFF CACHE BOOL "" FORCE)
set(KernelDebugBuild ON CACHE BOOL "" FORCE)
set(KernelPrinting ON CACHE BOOL "" FORCE)

# non-MCS master API -- the verified AArch64 kernel, and what the 0.4.x SaveCaller-based servers reuse.
set(KernelIsMCS OFF CACHE BOOL "" FORCE)

# GCC 15 workaround (from settings.cmake): musl weak_alias emits protected-visibility symbols the
# linker cannot copy-relocate -> force default visibility.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=default" CACHE STRING "" FORCE)

# The verified AArch64 configuration runs the kernel as an EL2 hypervisor (KernelArmHypervisorSupport
# ON) with FPU; the 0.4.x tree booted qemu-arm-virt with it ON too. Kept ON to develop against the
# verified config's shape (guests + root task stay EL0/EL1; the fault trap loop is unchanged).
set(KernelArmHypervisorSupport ON CACHE BOOL "" FORCE)

set(KernelRootCNodeSizeBits 16 CACHE STRING "" FORCE)
# Export the virtual counter to user (CNTVCT) -- the monotonic clock source a later phase reads.
set(KernelArmExportPCNTUser ON CACHE BOOL "" FORCE)

# muslcsys morecore heap for the root task (malloc/printf). 6MB as in the 0.4.x tree.
set(LibSel4MuslcSysMorecoreBytes 6291456 CACHE STRING "" FORCE)

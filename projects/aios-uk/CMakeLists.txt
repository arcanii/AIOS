#
# projects/aios-uk -- the AIOS userspace kernel as a real seL4 ROOT TASK (the real pal_sel4 port).
#
# Phase A (docs/PLAN_20260709_sel4_real_port.md): the HOST-AGNOSTIC kernel uk/kernel/aios_kernel.c --
# compiled BYTE-IDENTICAL from its own source (never copied) -- boots as an seL4 root task on
# qemu-arm-virt and prints its banner over serial via a real pal_host_write (seL4_DebugPutChar on the
# debug kernel). The real PAL lives in uk/pal/sel4/ (NOT the uk/pal/pal_sel4.c scaffold, which stays the
# `make PAL=sel4` link-purity canary). aios_kernel.c owns main(argc,argv); sel4runtime also enters at
# main, so the kernel is compiled with -Dmain=aios_kernel_main and boot.c provides the root-task main().
#
# Built only when -DAIOS_UK_BUILD=1 (which also skips the 0.4.x projects/aios rootserver); the two
# rootservers are mutually exclusive (one DeclareRootserver per image).
#
if(NOT AIOS_UK_BUILD)
    return()
endif()

cmake_minimum_required(VERSION 3.16.0)
project(aios-uk-rootserver C ASM)

set(AIOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(UK "${AIOS_ROOT}/uk")

# ---- Phase B: embed a guest for the fault-EP trap loop --------------------------------------------
# Build guest_hello as a FREESTANDING AIOS-ABI guest (aarch64 ET_EXEC with its OWN _start), EXACTLY
# like uk/Makefile's GUEST_CFLAGS, using the cross compiler CMake resolved. NOT add_executable() -- the
# default seL4 link set would pull in sel4runtime and replace _start with _sel4_start (it would stop
# being a pure AIOS-ABI guest that traps via svc). The output basename ("guest_hello") is the runtime
# image_name sel4utils matches against in the CPIO.
set(GUEST_HELLO_ELF ${CMAKE_CURRENT_BINARY_DIR}/guest_hello)
add_custom_command(
    OUTPUT ${GUEST_HELLO_ELF}
    COMMAND ${CMAKE_C_COMPILER} -O2 -Wall -Wextra -I${UK}/include
            -ffreestanding -nostdlib -static -fno-stack-protector -fno-asynchronous-unwind-tables
            -o ${GUEST_HELLO_ELF} ${UK}/guest/guest_hello.c
    DEPENDS ${UK}/guest/guest_hello.c ${UK}/include/aios_abi.h
    COMMENT "Phase B: build freestanding AIOS-ABI guest_hello (aarch64 ET_EXEC, own _start)"
    VERBATIM)
# MakeCPIO emits guests_cpio.o, which DEFINES the globals _cpio_archive / _cpio_archive_end that
# sel4utils reads directly (you never pass the archive -- only the image_name). Files are stored under
# their basename, so "guest_hello" is the name process_config_elf() resolves.
MakeCPIO(guests_cpio.o "${GUEST_HELLO_ELF}")

add_executable(aios-uk-sel4
    ${UK}/kernel/aios_kernel.c      # the host-agnostic kernel, unchanged (main -> aios_kernel_main)
    ${UK}/pal/sel4/boot.c           # the real seL4 PAL: root-task main + console + the trap loop
    guests_cpio.o                   # the embedded guest archive (defines _cpio_archive)
)

# aios_kernel.c includes ONLY aios_abi.h / aios_version.h / pal.h (+ stddef/stdint) -- so uk/include is
# the only AIOS include path it needs.
target_include_directories(aios-uk-sel4 PRIVATE ${UK}/include)
target_compile_definitions(aios-uk-sel4 PRIVATE ${AIOS_PLATFORM})
# -Dmain=aios_kernel_main renames the KERNEL's entry ONLY (build plumbing, same spirit as PAL
# selection; the source stays byte-identical). boot.c keeps its own main() -- the one sel4runtime calls
# -- so this MUST be per-source, never target-wide (that would rename boot.c's main too).
set_source_files_properties(${UK}/kernel/aios_kernel.c PROPERTIES COMPILE_DEFINITIONS "main=aios_kernel_main")

# The proven rootserver link set (from the 0.4.x aios_root). Phase A uses only sel4 + the C runtime +
# console, but linking the full dynamic set now avoids re-plumbing when families A/B need vka/vspace.
target_link_libraries(aios-uk-sel4
    sel4runtime sel4
    sel4allocman sel4vka sel4utils
    sel4simple sel4simple-default
    sel4platsupport sel4muslcsys muslc
    cpio elf)                       # Phase B: sel4utils loads the guest ELF via libcpio + libelf

DeclareRootserver(aios-uk-sel4)

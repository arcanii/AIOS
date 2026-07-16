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

# ---- embed the freestanding AIOS-ABI guests for the fault-EP trap loop ----------------------------
# Build each guest as an aarch64 ET_EXEC with its OWN _start (EXACTLY uk/Makefile's GUEST_CFLAGS), using
# the cross compiler CMake resolved. NOT add_executable() -- the default seL4 link set would pull in
# sel4runtime and replace _start with _sel4_start (it would stop being a pure AIOS-ABI guest that traps
# via svc). The output basename is the runtime image_name sel4utils matches against in the CPIO.
#   guest_hello -- Phase B (WRITE + EXIT);  guest_mmap -- Phase C.1 (mmap a region, pattern round-trip);
#   guest_fork  -- Phase C.2 (fork + wait: eager VSpace copy, two-return, isolation of data/stack/mmap);
#   guest_exec + guest_execd -- Phase C.3 (exec: failed-exec keeps the old image; argv/envp staging);
#   guest_tarfs -- Phase C.4 (tarfs file ops + exec of a REAL sbase binary out of aiosroot.tar);
#   guest_pipe -- Phase C.5 (a pipe: writer parks on a full ring, reader wakes it, EOF at close);
#   guest_fsbreadth -- Phase D.1 (getdents + *at + chdir/getcwd + the CNTPCT clock).
set(UK_GUESTS guest_hello guest_mmap guest_fork guest_exec guest_execd guest_tarfs guest_pipe guest_fsbreadth)
set(GUEST_ELFS "")
foreach(g ${UK_GUESTS})
    set(elf ${CMAKE_CURRENT_BINARY_DIR}/${g})
    add_custom_command(
        OUTPUT ${elf}
        COMMAND ${CMAKE_C_COMPILER} -O2 -Wall -Wextra -I${UK}/include
                -ffreestanding -nostdlib -static -fno-stack-protector -fno-asynchronous-unwind-tables
                -o ${elf} ${UK}/guest/${g}.c
        DEPENDS ${UK}/guest/${g}.c ${UK}/include/aios_abi.h
        COMMENT "build freestanding AIOS-ABI guest ${g} (aarch64 ET_EXEC, own _start)"
        VERBATIM)
    list(APPEND GUEST_ELFS ${elf})
endforeach()
# MakeCPIO emits guests_cpio.o, which DEFINES the globals _cpio_archive / _cpio_archive_end. Files
# are stored under their basename. Phase C.4 also embeds uk/aiosroot.tar -- the vendored dash+sbase
# WORLD the Linux line builds (make all + mkaiosroot.sh in uk/, run on the Linux side; it is a build
# product, not tracked -- build it there first if missing). boot.c mounts it as the read-only tarfs.
if(NOT EXISTS ${UK}/aiosroot.tar)
    message(FATAL_ERROR "uk/aiosroot.tar missing -- build the Linux line first (uk: make all && sh mkaiosroot.sh)")
endif()
MakeCPIO(guests_cpio.o "${GUEST_ELFS};${UK}/aiosroot.tar")

add_executable(aios-uk-sel4
    ${UK}/kernel/aios_kernel.c      # the host-agnostic kernel, unchanged (main -> aios_kernel_main)
    ${UK}/pal/sel4/boot.c           # the real seL4 PAL: root-task main + console + the trap loop
    ${UK}/pal/sel4/tarfs.c          # Phase C.4: the read-only tarfs over the embedded aiosroot.tar
    ${UK}/pal/sel4/pipe.c           # Phase C.5: the in-root-task pipe byte ring
    guests_cpio.o                   # the embedded archive: dev guests + aiosroot.tar
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

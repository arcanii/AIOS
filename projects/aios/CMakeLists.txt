cmake_minimum_required(VERSION 3.16.0)
project(aios-rootserver C ASM)

set(AIOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

add_library(aios_posix STATIC
    ${AIOS_ROOT}/src/lib/aios_posix.c
    ${AIOS_ROOT}/src/lib/posix_file.c
    ${AIOS_ROOT}/src/lib/posix_stat.c
    ${AIOS_ROOT}/src/lib/posix_dir.c
    ${AIOS_ROOT}/src/lib/posix_proc.c
    ${AIOS_ROOT}/src/lib/posix_time.c
    ${AIOS_ROOT}/src/lib/posix_misc.c
    ${AIOS_ROOT}/src/lib/posix_thread.c
    ${AIOS_ROOT}/src/lib/posix_net.c
    ${AIOS_ROOT}/src/lib/posix_compat.c
    ${AIOS_ROOT}/src/lib/config_parser.c
    ${AIOS_ROOT}/src/lib/aios_entry.c
)
target_include_directories(aios_posix PUBLIC ${AIOS_ROOT}/src/lib ${AIOS_ROOT}/include ${AIOS_ROOT}/src/arch)
target_link_libraries(aios_posix sel4 sel4muslcsys)

macro(AiosChildApp name)
    add_executable(${name} ${AIOS_ROOT}/src/apps/${name}.c)
    target_link_libraries(${name}
        sel4runtime sel4 sel4muslcsys muslc
        sel4platsupport sel4utils sel4simple sel4simple-default
        sel4vka sel4allocman
    )
    target_include_directories(${name} PRIVATE ${AIOS_ROOT}/include ${AIOS_ROOT}/src/lib ${AIOS_ROOT}/src/arch)
endmacro()

macro(AiosPosixApp name)
    AiosChildApp(${name})
    target_link_libraries(${name} aios_posix)
    target_compile_definitions(${name} PRIVATE ${AIOS_PLATFORM})
    target_link_options(${name} PRIVATE
        -Wl,--wrap=main
        -Wl,--wrap=pthread_create
        -Wl,--wrap=pthread_join
        -Wl,--wrap=pthread_exit
        -Wl,--wrap=pthread_detach
        -Wl,--wrap=pthread_mutex_init
        -Wl,--wrap=pthread_mutex_lock
        -Wl,--wrap=pthread_mutex_unlock
        -Wl,--wrap=pthread_mutex_destroy
        -Wl,--wrap=getpwuid
        -Wl,--wrap=getpwnam
        -Wl,--wrap=getgrnam
        -Wl,--wrap=getgrgid
    )
endmacro()

# System services
AiosChildApp(auth_server)
# netd: MMU-isolated network daemon skeleton (DESIGN_NETD Stage 2). Pure leaf
# server, raw seL4 IPC (no aios_posix), like tty_server/auth_server. Only
# spawned + bundled into the CPIO when AIOS_NETD is ON (see below); the binary
# always builds so the skeleton stays compile-checked.
AiosChildApp(netd)
AiosChildApp(hello_child)
AiosChildApp(echo_server)
AiosChildApp(serial_server)
AiosChildApp(tty_server)
AiosChildApp(getty)
target_link_libraries(getty aios_posix)
AiosPosixApp(mini_shell)
AiosChildApp(crash_test)
AiosChildApp(blk_test)

# POSIX programs
AiosPosixApp(sysinfo)
AiosPosixApp(posix_test)
AiosPosixApp(posix_ps)
AiosPosixApp(posix_nice)
AiosPosixApp(posix_id)
AiosPosixApp(posix_help)
AiosPosixApp(posix_rm)
AiosPosixApp(posix_touch)
AiosPosixApp(posix_mkdir)
AiosPosixApp(aios_top)
AiosPosixApp(daemon)
AiosPosixApp(fork_test)
AiosPosixApp(pipe_test)
AiosPosixApp(writetest)
AiosPosixApp(diagtest)
AiosPosixApp(signal_test)
AiosPosixApp(sbase_test)
AiosPosixApp(shutdown)
AiosPosixApp(reboot)
AiosPosixApp(fatswap)
AiosPosixApp(sntp)
AiosPosixApp(fbshow)
AiosPosixApp(tcp_connect)
AiosPosixApp(netdiag)
AiosPosixApp(test_fswrite)
AiosPosixApp(test_smp)
AiosPosixApp(test_pipe_rw)
AiosPosixApp(test_hw)
AiosPosixApp(test_mprotect)
AiosPosixApp(test_mmap)
AiosPosixApp(test_ftruncate)
AiosPosixApp(hwinfo)

AiosPosixApp(posix_verify
    SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../../src/apps/posix_verify.c
)

# netd de-monolithization (DESIGN_NETD). ON (default, production) = the net stack
# -- socket server + TCP/UDP/DHCP + the NIC driver device half -- runs in the
# isolated, MMU-protected netd process (Stage 3 cutover, HW-verified on real
# GENET). OFF = the legacy path with the net stack in the root task, kept for
# regression testing; the flag-OFF trees (build-04, build-rpi4) must be configured
# with an explicit -DAIOS_NETD=OFF so they stay OFF now that the default is ON.
option(AIOS_NETD "Spawn the isolated netd process (DESIGN_NETD)" ON)
# Test-only: build netd so it comes up but never sends DEVD_READY, to exercise the
# DESIGN_NETD s8 degrade-and-continue path (a wedged netd init must not brick boot).
# Default OFF; turn ON only in a throwaway build (e.g. build-netd-degrade).
option(AIOS_NETD_TEST_DEGRADE "netd skips DEVD_READY (degrade-path gate)" OFF)

# Boot services only in CPIO (launched before filesystem)
set(BOOT_APPS tty_server auth_server)
# netd joins the CPIO ONLY when AIOS_NETD is ON, so flag-OFF images are unchanged.
if(AIOS_NETD)
    list(APPEND BOOT_APPS netd)
endif()
set(CPIO_FILES "")
foreach(app ${BOOT_APPS})
    if(CPIO_FILES)
        set(CPIO_FILES "${CPIO_FILES};$<TARGET_FILE:${app}>")
    else()
        set(CPIO_FILES "$<TARGET_FILE:${app}>")
    endif()
endforeach()
MakeCPIO(apps_cpio.o "${CPIO_FILES}")

# All other programs built but loaded from disk via /bin

# Platform-specific driver sources
if(AIOS_PLATFORM STREQUAL "PLAT_QEMU_VIRT")
    set(PLAT_SOURCES
        ${AIOS_ROOT}/src/plat/qemu-virt/plat_virtio_probe.c
        ${AIOS_ROOT}/src/plat/qemu-virt/blk_virtio.c
        ${AIOS_ROOT}/src/plat/qemu-virt/net_virtio.c
        ${AIOS_ROOT}/src/plat/qemu-virt/display_ramfb.c
        ${AIOS_ROOT}/src/plat/qemu-virt/pcie_ecam.c
    )
elseif(AIOS_PLATFORM STREQUAL "PLAT_RPI4")
    set(PLAT_SOURCES
        ${AIOS_ROOT}/src/plat/rpi4/blk_emmc.c
        ${AIOS_ROOT}/src/plat/rpi4/net_genet.c
        ${AIOS_ROOT}/src/plat/rpi4/display_vc.c
        ${AIOS_ROOT}/src/plat/rpi4/pcie_brcmstb.c
    )
else()
    message(FATAL_ERROR "Unknown AIOS_PLATFORM: ${AIOS_PLATFORM}")
endif()

add_executable(aios_root
    ${AIOS_ROOT}/src/aios_root.c
    ${AIOS_ROOT}/src/cpu_gov.c
    ${AIOS_ROOT}/src/cpuacct.c
    ${AIOS_ROOT}/src/ext2.c
    ${AIOS_ROOT}/src/vfs.c
    ${AIOS_ROOT}/src/procfs.c
    ${AIOS_ROOT}/src/aios_log.c
    ${AIOS_ROOT}/src/blk_cache.c
    ${AIOS_ROOT}/src/fat32.c
    ${AIOS_ROOT}/src/filehits.c
    ${AIOS_ROOT}/src/boot/boot_warmup.c
    ${AIOS_ROOT}/src/process/fork.c
    ${AIOS_ROOT}/src/process/reap.c
    ${AIOS_ROOT}/src/process/cow.c
    ${AIOS_ROOT}/src/servers/thread_server.c
    ${AIOS_ROOT}/src/servers/fs_server.c
    ${AIOS_ROOT}/src/servers/exec_server.c
    ${AIOS_ROOT}/src/servers/pipe_server.c
    ${AIOS_ROOT}/src/servers/serverstats.c
    ${AIOS_ROOT}/src/servers/core_warm.c
    ${AIOS_ROOT}/src/servers/fabric_warm.c
    ${AIOS_ROOT}/src/servers/watchdog.c
    ${AIOS_ROOT}/src/servers/flush_server.c
    ${AIOS_ROOT}/src/boot/spawn_util.c
    ${AIOS_ROOT}/src/boot/boot_fs_init.c
    ${AIOS_ROOT}/src/lib/vka_audit.c
    ${AIOS_ROOT}/src/boot/boot_services.c
    ${AIOS_ROOT}/src/boot/spawn_netd.c
    ${AIOS_ROOT}/src/boot/boot_net_init.c
    ${AIOS_ROOT}/src/boot/boot_display_init.c
    ${AIOS_ROOT}/src/boot/fb_console.c
    ${AIOS_ROOT}/src/boot/boot_dtb.c
    ${AIOS_ROOT}/src/boot/boot_device_map.c
    ${AIOS_ROOT}/src/boot/boot_config.c
    ${AIOS_ROOT}/src/lib/config_parser.c
    ${AIOS_ROOT}/src/servers/display_server.c
    ${AIOS_ROOT}/src/servers/net_server.c
    ${AIOS_ROOT}/src/net/net_stack.c
    ${AIOS_ROOT}/src/net/net_tcp.c
    ${AIOS_ROOT}/src/net/net_dhcp.c
    ${AIOS_ROOT}/src/crypto/crypto_chacha20.c
    ${AIOS_ROOT}/src/crypto/entropy_collect.c
    ${AIOS_ROOT}/src/crypto/crypto_server.c
    ${AIOS_ROOT}/src/usb/xhci.c
    ${AIOS_ROOT}/src/gpu/v3d.c
    ${AIOS_ROOT}/src/gpu/v3d_cl.c
    ${AIOS_ROOT}/src/gpu/v3d_cube.c
    ${PLAT_SOURCES}
    apps_cpio.o
)
target_link_libraries(aios_root sel4runtime sel4
    sel4allocman sel4vka sel4utils
    sel4simple sel4simple-default
    sel4platsupport sel4muslcsys muslc
    cpio elf fdt)
target_include_directories(aios_root PRIVATE ${AIOS_ROOT}/include ${AIOS_ROOT}/src/arch ${AIOS_ROOT}/src ${AIOS_ROOT}/src/crypto)
target_compile_definitions(aios_root PRIVATE ${AIOS_PLATFORM})
if(AIOS_NETD)
    target_compile_definitions(aios_root PRIVATE AIOS_NETD)
    message(STATUS "AIOS_NETD=ON: bundling + spawning the isolated netd skeleton (DESIGN_NETD Stage 2)")
endif()
target_link_options(aios_root PRIVATE -Wl,--no-copy-dt-needed-entries)

# netd Stage 3 (DESIGN_NETD s9): compile the net stack + the platform driver dev
# half into the netd process under NETD_BUILD. The skeleton main (netd.c) does not
# call net_server_fn yet (Stage 3 wires that) -- compiling+linking it here is the
# isolation proof: if the net stack referenced a root-only symbol the netd_shim
# does not provide (e.g. the vka `vka` global), the netd link would FAIL. Root
# keeps its OWN full copy of the same sources for the in-root (flag-OFF) path; the
# two binaries never share these objects. Built unconditionally (like the netd
# skeleton) so every build compile-checks the NETD_BUILD path, flag on or off.
target_sources(netd PRIVATE
    ${AIOS_ROOT}/src/servers/net_server.c
    ${AIOS_ROOT}/src/net/net_stack.c
    ${AIOS_ROOT}/src/net/net_tcp.c
    ${AIOS_ROOT}/src/net/net_dhcp.c
    ${AIOS_ROOT}/src/apps/netd_shim.c
)
if(AIOS_PLATFORM STREQUAL "PLAT_QEMU_VIRT")
    target_sources(netd PRIVATE ${AIOS_ROOT}/src/plat/qemu-virt/net_virtio.c)
elseif(AIOS_PLATFORM STREQUAL "PLAT_RPI4")
    target_sources(netd PRIVATE ${AIOS_ROOT}/src/plat/rpi4/net_genet.c)
endif()
target_compile_definitions(netd PRIVATE NETD_BUILD ${AIOS_PLATFORM})
if(AIOS_NETD_TEST_DEGRADE)
    target_compile_definitions(netd PRIVATE NETD_TEST_NO_READY)
    message(STATUS "AIOS_NETD_TEST_DEGRADE=ON: netd will skip DEVD_READY (degrade gate)")
endif()
target_include_directories(netd PRIVATE ${AIOS_ROOT}/src)

DeclareRootserver(aios_root)

add_custom_command(
    TARGET aios_root PRE_BUILD
    COMMAND bash ${AIOS_ROOT}/scripts/bump-build.sh
    WORKING_DIRECTORY ${AIOS_ROOT}
    COMMENT "Incrementing build number"
)

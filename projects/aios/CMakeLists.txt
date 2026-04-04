cmake_minimum_required(VERSION 3.16.0)
project(aios-rootserver C ASM)

set(AIOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

add_library(aios_posix STATIC ${AIOS_ROOT}/src/lib/aios_posix.c)
target_include_directories(aios_posix PUBLIC ${AIOS_ROOT}/src/lib ${AIOS_ROOT}/include)
target_link_libraries(aios_posix sel4 sel4muslcsys)

macro(AiosChildApp name)
    add_executable(${name} ${AIOS_ROOT}/src/apps/${name}.c)
    target_link_libraries(${name}
        sel4runtime sel4 sel4muslcsys muslc
        sel4platsupport sel4utils sel4simple sel4simple-default
        sel4vka sel4allocman
    )
    target_include_directories(${name} PRIVATE ${AIOS_ROOT}/include ${AIOS_ROOT}/src/lib)
endmacro()

macro(AiosPosixApp name)
    AiosChildApp(${name})
    target_link_libraries(${name} aios_posix)
    target_link_options(${name} PRIVATE -Wl,--wrap=main)
endmacro()

# System services
AiosChildApp(auth_server)
AiosChildApp(hello_child)
AiosChildApp(echo_server)
AiosChildApp(serial_server)
AiosChildApp(tty_server)
AiosChildApp(mini_shell)
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

# Boot services only in CPIO (launched before filesystem)
set(BOOT_APPS tty_server auth_server mini_shell)
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

add_executable(aios_root
    ${AIOS_ROOT}/src/aios_root.c
    ${AIOS_ROOT}/src/ext2.c
    ${AIOS_ROOT}/src/vfs.c
    ${AIOS_ROOT}/src/procfs.c
    ${AIOS_ROOT}/src/aios_log.c
    apps_cpio.o
)
target_link_libraries(aios_root
    sel4runtime sel4
    sel4allocman sel4vka sel4utils
    sel4simple sel4simple-default
    sel4platsupport sel4muslcsys muslc
    cpio elf
)
target_include_directories(aios_root PRIVATE ${AIOS_ROOT}/include)
target_link_options(aios_root PRIVATE -Wl,--no-copy-dt-needed-entries)

DeclareRootserver(aios_root)

add_custom_command(
    TARGET aios_root PRE_BUILD
    COMMAND bash ${AIOS_ROOT}/scripts/bump-build.sh
    WORKING_DIRECTORY ${AIOS_ROOT}
    COMMENT "Incrementing build number"
)

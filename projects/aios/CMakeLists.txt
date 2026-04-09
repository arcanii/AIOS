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
    ${AIOS_ROOT}/src/lib/aios_entry.c
)
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

AiosPosixApp(posix_verify
    SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../../src/apps/posix_verify.c
)

# Boot services only in CPIO (launched before filesystem)
set(BOOT_APPS tty_server auth_server)
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
    ${AIOS_ROOT}/src/process/fork.c
    ${AIOS_ROOT}/src/process/reap.c
    ${AIOS_ROOT}/src/servers/thread_server.c
    ${AIOS_ROOT}/src/servers/fs_server.c
    ${AIOS_ROOT}/src/servers/exec_server.c
    ${AIOS_ROOT}/src/servers/pipe_server.c
    ${AIOS_ROOT}/src/boot/blk_io.c
    ${AIOS_ROOT}/src/boot/spawn_util.c
    ${AIOS_ROOT}/src/boot/boot_fs_init.c
    ${AIOS_ROOT}/src/boot/boot_log_init.c
    ${AIOS_ROOT}/src/lib/vka_audit.c
    ${AIOS_ROOT}/src/boot/boot_services.c
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

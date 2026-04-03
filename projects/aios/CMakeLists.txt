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
endmacro()

# System services
AiosChildApp(hello_child)
AiosChildApp(echo_server)
AiosChildApp(serial_server)
AiosChildApp(mini_shell)
AiosChildApp(crash_test)
AiosChildApp(blk_test)

# POSIX programs
AiosPosixApp(sysinfo)
AiosPosixApp(aios_cat)
AiosPosixApp(posix_test)
AiosPosixApp(posix_cat)
AiosPosixApp(posix_echo)
AiosPosixApp(posix_wc)
AiosPosixApp(posix_head)
AiosPosixApp(posix_uname)
AiosPosixApp(posix_ls)
AiosPosixApp(posix_ps)
AiosPosixApp(posix_nice)
AiosPosixApp(posix_true)
AiosPosixApp(posix_false)
AiosPosixApp(posix_pwd)
AiosPosixApp(posix_basename)
AiosPosixApp(posix_dirname)
AiosPosixApp(posix_whoami)
AiosPosixApp(posix_id)
AiosPosixApp(posix_date)
AiosPosixApp(posix_env)
AiosPosixApp(posix_yes)
AiosPosixApp(posix_tee)
AiosPosixApp(posix_sort)
AiosPosixApp(posix_grep)

set(ALL_APPS
    hello_child echo_server serial_server mini_shell crash_test blk_test
    sysinfo aios_cat posix_test
    posix_cat posix_echo posix_wc posix_head posix_uname posix_ls posix_ps
    posix_nice posix_true posix_false posix_pwd posix_basename posix_dirname
    posix_whoami posix_id posix_date posix_env posix_yes posix_tee
    posix_sort posix_grep
)

set(CPIO_FILES "")
foreach(app ${ALL_APPS})
    if(CPIO_FILES)
        set(CPIO_FILES "${CPIO_FILES};$<TARGET_FILE:${app}>")
    else()
        set(CPIO_FILES "$<TARGET_FILE:${app}>")
    endif()
endforeach()

MakeCPIO(apps_cpio.o "${CPIO_FILES}")

add_executable(aios_root
    ${AIOS_ROOT}/src/aios_root.c
    ${AIOS_ROOT}/src/ext2.c
    ${AIOS_ROOT}/src/vfs.c
    ${AIOS_ROOT}/src/procfs.c
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

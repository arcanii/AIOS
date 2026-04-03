cmake_minimum_required(VERSION 3.16.0)
project(aios-rootserver C ASM)

set(AIOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

# AIOS POSIX shim library
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

# System services (no aios_posix)
AiosChildApp(hello_child)
AiosChildApp(echo_server)
AiosChildApp(serial_server)
AiosChildApp(mini_shell)
AiosChildApp(crash_test)
AiosChildApp(blk_test)

# POSIX programs (with aios_posix shim)
AiosChildApp(sysinfo)
AiosChildApp(aios_cat)
AiosChildApp(posix_test)
AiosChildApp(posix_cat)
AiosChildApp(posix_echo)
AiosChildApp(posix_wc)
AiosChildApp(posix_head)
AiosChildApp(posix_uname)
AiosChildApp(posix_ls)
AiosChildApp(posix_ps)

target_link_libraries(sysinfo aios_posix)
target_link_libraries(aios_cat aios_posix)
target_link_libraries(posix_test aios_posix)
target_link_libraries(posix_cat aios_posix)
target_link_libraries(posix_echo aios_posix)
target_link_libraries(posix_wc aios_posix)
target_link_libraries(posix_head aios_posix)
target_link_libraries(posix_uname aios_posix)
target_link_libraries(posix_ls aios_posix)
target_link_libraries(posix_ps aios_posix)

MakeCPIO(apps_cpio.o
    "$<TARGET_FILE:hello_child>;$<TARGET_FILE:echo_server>;$<TARGET_FILE:serial_server>;$<TARGET_FILE:mini_shell>;$<TARGET_FILE:crash_test>;$<TARGET_FILE:blk_test>;$<TARGET_FILE:sysinfo>;$<TARGET_FILE:aios_cat>;$<TARGET_FILE:posix_test>;$<TARGET_FILE:posix_cat>;$<TARGET_FILE:posix_echo>;$<TARGET_FILE:posix_wc>;$<TARGET_FILE:posix_head>;$<TARGET_FILE:posix_uname>;$<TARGET_FILE:posix_ls>;$<TARGET_FILE:posix_ps>"
)

add_executable(aios_root ${AIOS_ROOT}/src/aios_root.c ${AIOS_ROOT}/src/ext2.c ${AIOS_ROOT}/src/vfs.c ${AIOS_ROOT}/src/procfs.c apps_cpio.o)
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

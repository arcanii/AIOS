cmake_minimum_required(VERSION 3.16.0)
project(aios-rootserver C ASM)

set(AIOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

macro(AiosChildApp name)
    add_executable(${name} ${AIOS_ROOT}/src/apps/${name}.c)
    target_link_libraries(${name}
        sel4runtime sel4 sel4muslcsys muslc
        sel4platsupport sel4utils sel4simple sel4simple-default
        sel4vka sel4allocman
    )
    target_include_directories(${name} PRIVATE ${AIOS_ROOT}/include)
endmacro()

AiosChildApp(hello_child)
AiosChildApp(echo_server)
AiosChildApp(serial_server)
AiosChildApp(mini_shell)
AiosChildApp(crash_test)

MakeCPIO(apps_cpio.o
    "$<TARGET_FILE:hello_child>;$<TARGET_FILE:echo_server>;$<TARGET_FILE:serial_server>;$<TARGET_FILE:mini_shell>;$<TARGET_FILE:crash_test>"
)

add_executable(aios_root ${AIOS_ROOT}/src/aios_root.c apps_cpio.o)
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

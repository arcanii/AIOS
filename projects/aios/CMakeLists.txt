#
# AIOS root task + child apps
#
cmake_minimum_required(VERSION 3.16.0)
project(aios-rootserver C ASM)

set(AIOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

# Child app 1: hello
add_executable(hello_child ${AIOS_ROOT}/src/apps/hello_child.c)
target_link_libraries(hello_child
    sel4runtime sel4 sel4muslcsys muslc
    sel4platsupport sel4utils sel4simple sel4simple-default
    sel4vka sel4allocman
)
target_include_directories(hello_child PRIVATE ${AIOS_ROOT}/include)

# Child app 2: echo server (IPC demo)
add_executable(echo_server ${AIOS_ROOT}/src/apps/echo_server.c)
target_link_libraries(echo_server
    sel4runtime sel4 sel4muslcsys muslc
    sel4platsupport sel4utils sel4simple sel4simple-default
    sel4vka sel4allocman
)
target_include_directories(echo_server PRIVATE ${AIOS_ROOT}/include)

# Embed all children into CPIO
MakeCPIO(apps_cpio.o "$<TARGET_FILE:hello_child>;$<TARGET_FILE:echo_server>")

# Root task
add_executable(aios_root ${AIOS_ROOT}/src/aios_root.c apps_cpio.o)
target_link_libraries(aios_root
    sel4runtime sel4
    sel4allocman sel4vka sel4utils
    sel4simple sel4simple-default
    sel4platsupport
    sel4muslcsys muslc
    cpio elf
)
target_include_directories(aios_root PRIVATE ${AIOS_ROOT}/include)
target_link_options(aios_root PRIVATE -Wl,--no-copy-dt-needed-entries)

DeclareRootserver(aios_root)

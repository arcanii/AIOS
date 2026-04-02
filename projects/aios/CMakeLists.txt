#
# AIOS root task project
#
cmake_minimum_required(VERSION 3.16.0)
project(aios-rootserver C ASM)

# Our root task source
add_executable(aios_root ${CMAKE_CURRENT_LIST_DIR}/../../src/aios_root.c)

target_link_libraries(aios_root
    sel4runtime sel4
    sel4allocman sel4vka sel4utils
    sel4simple sel4simple-default
    sel4platsupport
    sel4muslcsys muslc
)

target_include_directories(aios_root PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../include
)

# GCC 15 workaround: musllibc protected symbols
target_link_options(aios_root PRIVATE -Wl,--no-copy-dt-needed-entries)

DeclareRootserver(aios_root)

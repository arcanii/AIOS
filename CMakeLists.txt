#
# AIOS 0.4.x — Root task on bare seL4
#
cmake_minimum_required(VERSION 3.16.0)

# Load settings BEFORE project() -- sets platform, arch, etc.
# Use -DAIOS_SETTINGS=settings-rpi4.cmake for RPi4 builds.
if(NOT DEFINED AIOS_SETTINGS)
    set(AIOS_SETTINGS "settings.cmake")
endif()
include(${AIOS_SETTINGS})

project(aios NONE)

# Emit compile_commands.json in the build dir for clangd / IDE tooling.
# Symlink it to the repo root: ln -sf build-04/compile_commands.json .
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

# Use seL4's standard build infrastructure
include(tools/seL4/cmake-tool/all.cmake)

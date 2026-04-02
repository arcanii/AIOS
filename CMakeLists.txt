#
# AIOS 0.4.x — Root task on bare seL4
#
cmake_minimum_required(VERSION 3.16.0)

# Load settings BEFORE project() — sets platform, arch, etc.
include(settings.cmake)

project(aios NONE)

# Use seL4's standard build infrastructure
include(tools/seL4/cmake-tool/all.cmake)

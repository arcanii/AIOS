# AIOS dependency manifest

Every external source tree AIOS builds against, pinned to the exact commit the
project is developed and tested at. `./build_environment.sh` consumes this
table (it greps the pin lines below), clones each repo at its pin, and applies
the patch set from `deps/patches/`. Update a pin only together with a
successful QEMU + hardware verification, and re-capture patches with
`./build_environment.sh --capture-patches` if you change a dep tree.

"Layout" is where the tree lives relative to this repo: `deps/` trees sit
inside the repo (gitignored, fetched by the script); `sibling` trees sit next
to the AIOS checkout (`../<name>`, overridable with `AIOS_DEPS_ROOT`).

## seL4 ecosystem (layout: deps/)

| name | pin | repo | patch |
|------|-----|------|-------|
| seL4-kernel | `3f590a602` | https://github.com/seL4/seL4.git | `seL4-kernel.patch` (RPi4 DTS: kernel/serial placement) |
| musllibc | `9798aedb` | https://github.com/seL4/musllibc.git | `musllibc.patch` (GCC 15 symbol visibility) |
| seL4_libs | `4ce71fc` | https://github.com/seL4/seL4_libs.git | `seL4_libs.patch` (morecore size, vspace, muslcsys) |
| seL4_tools | `83eeeda` | https://github.com/seL4/seL4_tools.git | `seL4_tools.patch` (elfloader: RPi4 mini-UART console, boot fixes) + `files/elfloader-diag_fb_debug.h` |
| sel4runtime | `86489cf` | https://github.com/seL4/sel4runtime.git | none |
| util_libs | `c8f9ea7` | https://github.com/seL4/util_libs.git | none |

`deps/kernel` is a symlink to the seL4-kernel checkout (the build system
resolves `deps/kernel/gcc.cmake` etc. through it).

## Userspace third party (layout: sibling)

| name | pin | repo | patch |
|------|-----|------|-------|
| sbase | `8b842c7` | https://git.suckless.org/sbase | none |
| dash | `057cd65` | https://github.com/tklauser/dash.git | `files/dash-config.h` -> `src/config.h` (+ generated headers, see script) |
| zsh | `fd57b65` | https://github.com/zsh-users/zsh.git | `files/zsh-termcap.h` -> `Src/termcap.h` |
| tcc | `98765e5e` | https://github.com/TinyCC/tinycc.git | `tcc.patch` (arm64 codegen/linker for AIOS) |
| mbedtls | `22098d4` | https://github.com/Mbed-TLS/mbedtls.git | `mbedtls.patch` (config for AIOS sshd) |

## Host toolchain

See `build_environment.sh --check` and the README Prerequisites section:
aarch64 cross GCC (Homebrew `aarch64-unknown-linux-gnu`, prefix
`aarch64-linux-gnu-` on Linux), cmake, ninja, python3, qemu-system-aarch64,
gnu-sed, texinfo, dtc, libxml2, e2fsprogs.

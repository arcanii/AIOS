# AIOS Self-Hosted Compilation Design (tcc)

## Executive Summary

Port tcc (Tiny C Compiler) to AIOS as the first step toward self-hosted
development. tcc is a single-pass C compiler with built-in preprocessor,
assembler, and linker in ~30K SLOC. It has an AArch64 backend, requires
only basic POSIX file I/O, and can compile itself. The milestone: compile
and execute a C program entirely within AIOS.

## Why tcc

| | tcc | gcc | clang |
|---|---|---|---|
| SLOC | ~30K | ~15M | ~10M |
| Self-contained | cc + as + ld | Needs binutils, libgcc | Needs LLVM |
| Cross-compile effort | Similar to dash (~2-3 sessions) | Months | Months |
| AArch64 backend | Yes (arm64-gen.c) | Full | Full |
| Self-hosting | Yes | Yes (hours to build) | No |
| Binary size | ~200-300KB | ~50MB+ | ~100MB+ |
| Dependencies | Minimal POSIX | Heavy | Heavy |
| Forks for preprocessing | No (built-in) | Yes (cpp) | Yes |
| Optimization | Minimal (expression-level) | Full (-O0 to -O3) | Full |

tcc is the only realistic option for a first compiler on a research OS.
It can be cross-compiled with the same aios-cc toolchain used for dash.
Once running, it can compile itself -- achieving true self-hosting.

## What tcc Unlocks

| Capability | Without tcc | With tcc |
|---|---|---|
| Compile C programs | Host only (cross-compile) | On-device |
| Build sbase tools | Host aios-cc | Inside AIOS |
| Modify and recompile shell | Host rebuild + disk image | Edit + tcc + test |
| Compile new programs | Cross-compile pipeline | Write + compile + run |
| Self-hosting | No | tcc compiles tcc |
| Education/research | Black-box binaries | Observable compilation |

## Architecture

```
    User types: tcc -o /tmp/hello /tmp/hello.c
        |
    dash (login shell)
        |  fork + exec /bin/tcc
        v
    tcc process (isolated VSpace)
        |
        +-- Preprocessor (built-in, no fork)
        |     #include resolution: /usr/include, /usr/lib/tcc/include
        |
        +-- Parser (single-pass C89/C99)
        |
        +-- Code generator (arm64-gen.c)
        |     Emits AArch64 machine code directly
        |
        +-- Assembler (built-in)
        |     Processes inline asm (GAS syntax)
        |
        +-- Linker (built-in)
        |     Reads crt0.o, libc.a, libtcc1.a
        |     Resolves symbols, applies relocations
        |     Writes ELF binary
        |
        +-- write(/tmp/hello, elf_data)
        |
    tcc exits
        |
    dash: /tmp/hello
        |  fork + exec /tmp/hello
        v
    hello process: "Hello from AIOS-compiled C!"
```

No external tools needed. No fork during compilation. Single-process,
single-pass from source to executable.

## AIOS Prerequisites

### Already Working

| Requirement | Status | Notes |
|---|---|---|
| fork + exec + waitpid | Done | tcc runs as a child process |
| File read (open, read, close) | Done | tcc reads .c and .h files |
| File write (create, write) | Done | tcc writes .o and ELF output |
| File exec after write | Done | Compile then run works |
| /tmp writable | Done | Scratch space for output |
| ext2 write (create file) | Done | Multi-block, indirect blocks |
| /dev/null | Done | tcc may redirect stderr |
| malloc / free | Done (musl) | tcc internal allocation |
| Environment variables | Done | tcc uses $PATH, $C_INCLUDE_PATH |
| Pipes | Done | `tcc -run hello.c | cat` |
| Signal handling | Done | Ctrl-C during compilation |
| 128+ programs on disk | Done | Existing sbase tools |

### Needs Work

| Requirement | Current | Needed | Effort |
|---|---|---|---|
| mmap MAP_ANONYMOUS | Stub (returns addr) | Real allocation | Medium |
| mmap PROT_EXEC | Not implemented | For -run mode only | Medium (defer) |
| mprotect | Stub (returns 0) | For -run mode only | Medium (defer) |
| File stat (st_size) | Working | tcc uses fstat for file size | Done |
| Large file write | ~512 bytes per IPC | Need chunked write or SHM | Small |
| Header files on disk | Not present | Install musl headers to /usr/include | Small |
| libc.a on disk | Not present | Install musl libc.a to /usr/lib | Small |
| crt0.o on disk | Not present | Install CRT objects | Small |
| libtcc1.a | Not present | Cross-compile tcc runtime lib | Small |

### Not Needed (for initial port)

| Feature | Why not needed |
|---|---|
| mmap file-backed | tcc reads files via read(), not mmap |
| dlopen / dynamic linking | Static linking only |
| -run mode (JIT) | Defer -- compile to disk first |
| Temp files in /tmp | tcc can compile single-file without temps |
| C++ support | tcc is C only |
| Debugger support | DWARF output optional |

## File Layout on Disk

```
/bin/tcc                          tcc binary (~250KB)
/usr/lib/tcc/include/             tcc-specific headers
    stdarg.h                      tcc built-in (varargs via __builtin)
    stddef.h                      tcc built-in
    stdbool.h                     tcc built-in
    float.h                       tcc built-in
    tcclib.h                      tcc minimal libc header
/usr/include/                     musl libc headers
    stdio.h
    stdlib.h
    string.h
    unistd.h
    ...                           (~50 essential headers)
/usr/lib/
    libc.a                        musl static library
    libtcc1.a                     tcc runtime library
    crt1.o                        C runtime startup
    crti.o                        Init section
    crtn.o                        Fini section
```

Total disk footprint: ~2-3MB (tcc binary + headers + libraries).
Current ext2 disk is 128MB with ~77MB free -- plenty of space.

## Cross-Compilation Plan

### Phase 1: Clone and Configure

```
cd ~/Desktop/github_repos
git clone https://repo.or.cz/tinycc.git tcc
cd tcc
```

Create `config.h` for AIOS (similar to dash config.h):

```c
/* AIOS tcc cross-compile configuration */
#define TCC_TARGET_ARM64 1
#define CONFIG_TCC_STATIC 1
#define CONFIG_TCC_SYSINCLUDEPATHS "/usr/include"
#define CONFIG_TCC_LIBPATHS "/usr/lib"
#define CONFIG_TCC_CRTPREFIX "/usr/lib"
#define CONFIG_TCC_ELFINTERP ""
#define TCC_VERSION "0.9.28"
#define GCC_MAJOR 15
#define GCC_MINOR 0
#define ONE_SOURCE 1
```

Key defines:
- `ONE_SOURCE`: Compiles all of tcc from a single libtcc.c include
  (includes tccpp.c, tccelf.c, arm64-gen.c, etc.)
- `TCC_TARGET_ARM64`: Selects AArch64 code generator
- `CONFIG_TCC_STATIC`: Static linking only (no dlopen)

### Phase 2: Cross-Compile with aios-cc

tcc supports ONE_SOURCE mode where libtcc.c #includes all other .c files:

```
cd ~/Desktop/github_repos/AIOS
TCC=~/Desktop/github_repos/tcc

./scripts/aios-cc \
    $TCC/tcc.c \
    -I $TCC -I $TCC/include \
    -include aios_tcc_config.h \
    -DONE_SOURCE -DTCC_TARGET_ARM64 \
    -DCONFIG_TCC_STATIC \
    -o build-04/sbase/tcc
```

ONE_SOURCE means we compile just tcc.c which includes libtcc.c which
includes everything else. This is the simplest build method and avoids
dealing with 20+ individual source files.

### Phase 3: Build Runtime Library

tcc needs libtcc1.a (small runtime for things like 64-bit division,
va_arg support on AArch64). Cross-compile from tcc/lib/:

```
cd ~/Desktop/github_repos/AIOS
TCC=~/Desktop/github_repos/tcc

aarch64-linux-gnu-gcc -c $TCC/lib/libtcc1.c \
    -I $TCC/include -o /tmp/libtcc1.o
aarch64-linux-gnu-ar rcs build-04/sbase/libtcc1.a /tmp/libtcc1.o
```

### Phase 4: Install Headers and Libraries

Add to mkdisk.py or disk/rootfs:

```
disk/rootfs/usr/include/          <- copy essential musl headers
disk/rootfs/usr/lib/libc.a        <- copy from build-04 musl output
disk/rootfs/usr/lib/libtcc1.a     <- from Phase 3
disk/rootfs/usr/lib/crt1.o        <- from build-04 musl output
disk/rootfs/usr/lib/crti.o        <- from build-04 musl output
disk/rootfs/usr/lib/crtn.o        <- from build-04 musl output
disk/rootfs/usr/lib/tcc/include/  <- copy tcc built-in headers
```

The musl headers and libraries are already built in
`build-04/projects/musllibc/build-temp/stage/`. We just need to
install them to the disk image.

### Phase 5: Test

```
# Inside AIOS (dash shell):
echo 'int main() { printf("Hello from tcc!\n"); return 0; }' > /tmp/hello.c
tcc -o /tmp/hello /tmp/hello.c
/tmp/hello
```

Expected output: `Hello from tcc!`

## Known Challenges

### C1: Large File Write

The current fs_server write path sends data via IPC message registers
(~512 bytes per call for FS_WRITE_FILE, ~900 bytes for SHM path).
tcc output binaries are typically 5-50KB. This requires multiple
write calls, which the POSIX shim already handles via the write()
loop in posix_file.c. May need chunked write optimization for
larger outputs.

### C2: MAP_ANONYMOUS for malloc

musl malloc uses mmap(MAP_ANONYMOUS) for large allocations. The current
AIOS mmap stub returns a fixed address without actually allocating
memory. For tcc to work, MAP_ANONYMOUS needs to return real pages:

```c
/* In posix_misc.c or new posix_mmap.c: */
case __NR_mmap:
    /* Allocate anonymous pages from VKA pool */
    int npages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    /* vka_alloc_frame for each page, map into process VSpace */
    return mapped_vaddr;
```

This is the most significant prerequisite. Without it, tcc cannot
allocate memory for its internal data structures when they exceed
the sbrk heap. The fix benefits all programs, not just tcc.

### C3: Header File Count

musl has ~200 header files. Installing all of them adds ~1MB to the
disk image. For the initial port, install only the ~50 headers that
tcc actually includes (stdio.h, stdlib.h, string.h, unistd.h,
stdint.h, fcntl.h, sys/types.h, sys/stat.h, errno.h, etc.).

### C4: tcc Include Path Discovery

tcc searches for headers in compiled-in paths. The AIOS build must
set CONFIG_TCC_SYSINCLUDEPATHS to match the disk layout. If tcc
cannot find headers, compilation fails with unhelpful errors.

### C5: ELF Buffer Size

The exec_server uses a 1MB static buffer for ELF loading. tcc output
is typically <50KB, well within limits. tcc itself is ~250KB, also
fine. But if tcc compiles a large program, the output must be <1MB.
This is a known limitation documented in AI_BRIEFING.md.

## Milestones

### M1: Cross-Compile tcc (~1 session)

- Clone tcc, create AIOS config.h
- Cross-compile with aios-cc (ONE_SOURCE mode)
- Verify binary: file tcc, size tcc
- No AIOS changes needed

### M2: Install Headers + Libraries (~1 session)

- Copy musl headers to disk/rootfs/usr/include/
- Copy musl libc.a, crt objects to disk/rootfs/usr/lib/
- Cross-compile libtcc1.a
- Copy tcc built-in headers to disk/rootfs/usr/lib/tcc/include/
- Update mkdisk.py to install /usr/ tree
- Test: boot AIOS, verify `ls /usr/include` works

### M3: Fix mmap MAP_ANONYMOUS (~1-2 sessions)

- Implement real MAP_ANONYMOUS in POSIX shim
- Allocate pages from VKA pool, map into process VSpace
- Track mapped regions for munmap cleanup
- Test: programs that malloc large buffers work

### M4: First Compilation (~1 session)

- Boot AIOS with tcc + headers + libraries on disk
- Write hello.c to /tmp
- Run `tcc -o /tmp/hello /tmp/hello.c`
- Execute /tmp/hello
- Debug any missing syscalls or headers

### M5: Self-Hosting (~1 session)

- Copy tcc source files to AIOS disk
- Run `tcc -o /tmp/tcc2 tcc.c -DONE_SOURCE -DTCC_TARGET_ARM64`
- tcc compiles itself inside AIOS
- Verify: `/tmp/tcc2 -o /tmp/hello /tmp/hello.c && /tmp/hello`

### M6: Build Tools Inside AIOS (ongoing)

- Compile sbase tools with tcc inside AIOS
- Compile dash with tcc inside AIOS
- Write and compile new programs without leaving AIOS

## Design Decisions

### D1: ONE_SOURCE Mode

tcc supports compiling the entire compiler from a single tcc.c entry
point (libtcc.c includes all other .c files). This simplifies the
cross-compile to a single aios-cc invocation, identical to the dash
build pattern. No Makefile or multi-step build needed.

### D2: Static Linking Only

Dynamic linking (dlopen, shared libraries) requires significant
infrastructure (ld.so, PLT/GOT, lazy binding). Static linking with
libc.a + libtcc1.a is sufficient and matches how all AIOS programs
are currently built.

### D3: Disable -run Mode Initially

tcc -run compiles and executes in memory without writing to disk.
This requires mmap with PROT_EXEC (executable memory pages). AIOS
does not support this yet. The standard compile-to-disk path
(tcc -o output input.c) works with basic file I/O only. -run mode
can be added later when mmap PROT_EXEC is implemented.

### D4: Minimal Header Set

Install ~50 essential musl headers rather than all ~200. This saves
disk space and reduces complexity. Headers can be added incrementally
as programs need them.

### D5: /usr/include and /usr/lib Layout

Follow the standard Unix layout. tcc has compiled-in search paths
that expect this layout. Using /usr/include (not /include) and
/usr/lib (not /lib) matches tcc defaults and avoids path hacking.

## Syscall Audit for tcc

tcc uses these syscalls (all present in AIOS unless noted):

```
open/openat     - read source, write output          DONE
read            - read source files                   DONE
write           - write object/ELF output             DONE
close           - cleanup                             DONE
lseek           - seek in output file                 DONE
fstat           - get file size for read buffer       DONE
unlink          - remove temp files                   DONE
exit            - normal termination                  DONE
brk             - small heap allocations (musl sbrk)  DONE
mmap(ANON)      - large allocations (musl malloc)     NEEDS WORK
munmap          - free large allocations              NEEDS WORK
mprotect        - -run mode only                      DEFER
getcwd          - include path resolution             DONE
stat            - check file existence                DONE
```

Only mmap/munmap need real implementation. Everything else works.

## Relationship to Networking

tcc and networking are independent priorities. tcc enables self-hosted
development; networking enables external connectivity. They can be
developed in parallel (different files, different subsystems). The
only shared prerequisite is mmap, which benefits both.

Recommended order:
1. tcc M1-M2 (cross-compile + install) -- no AIOS changes needed
2. mmap fix (M3) -- benefits tcc, networking, and all programs
3. tcc M4-M5 (first compile + self-hosting)
4. Networking M1-M2 (device detection + ping)

## QEMU Command

Same as standard AIOS boot -- no extra devices needed for tcc:

```bash
qemu-system-aarch64 \
    -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 -m 2G \
    -nographic -serial mon:stdio \
    -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

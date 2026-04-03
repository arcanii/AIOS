# AIOS 0.4.x Development Learnings

Hard-won knowledge from building a microkernel OS on bare seL4. Every item here cost debugging time.

## seL4 Kernel Configuration

### SMP Requires Hypervisor Mode
QEMU virt's PSCI implementation uses HVC without `virtualization=on`, but the seL4 elfloader explicitly rejects HVC for PSCI (`smp-psci.c` line 21-22). With `virtualization=on`, PSCI uses SMC and elfloader accepts it.

Required QEMU flags: `-machine virt,virtualization=on -smp 4`

Required kernel config:
```cmake
set(KernelArmHypervisorSupport ON CACHE BOOL "" FORCE)
set(KernelMaxNumNodes 4 CACHE STRING "" FORCE)
```

The kernel runs at EL2 (hypervisor mode). This uses slightly more RAM (~2 MB more for kernel structures).

### EL2 Timer Access (v0.4.23)
By default, seL4 traps `cntpct_el0` reads from userspace. Enable:
```cmake
set(KernelArmExportPCNTUser ON CACHE BOOL "" FORCE)
```
This sets `CNTHCTL_EL2.EL1PCTEN` and `CNTKCTL_EL1.EL0PCTEN`, allowing userspace to read the ARM generic timer counter and frequency registers. Without this, `clock_gettime()` hangs or traps.

### MCS Scheduler Is Broken with sel4utils
`sel4utils_configure_process` fails with MCS enabled — it cannot set up scheduling contexts properly. Use the classic (non-MCS) scheduler:
```cmake
set(KernelIsMCS OFF CACHE BOOL "" FORCE)
```

### Root CNode Size Matters
Default 12-bit CNode (4096 slots) runs out after spawning ~5 processes. For exec-from-shell workflows, use 16 bits:
```cmake
set(KernelRootCNodeSizeBits 16 CACHE STRING "" FORCE)
```
This gives 65536 capability slots — enough for dozens of process spawns.

### Debug Build Is Essential During Development
```cmake
set(KernelDebugBuild ON CACHE BOOL "" FORCE)
set(KernelPrinting ON CACHE BOOL "" FORCE)
```
Without these, faults are silent. With them, you get stack traces and fault addresses.

## Process Management

### Priority Discipline — All at 200
`seL4_Yield()` only yields to equal priority threads. If root is at 253 and shell at 100, root never yields and shell starves. All cooperating processes must be at the same priority (we use 200).

Set root priority early, before spawning anything:
```c
seL4_TCB_SetPriority(seL4_CapInitThreadTCB, seL4_CapInitThreadTCB, 200);
```

### Process Cleanup Is Mandatory
`sel4utils_destroy_process()` must be called after every process exit. Without it, VSpace, CSpace, TCB, and untyped memory leak. After ~5 spawns without cleanup, the system runs out of untyped memory.

### Fault Endpoint Cleanup (v0.4.28)
Each exec'd process gets a dedicated fault endpoint. This MUST be freed after the child exits:
```c
sel4utils_destroy_process(&proc, &vka);
vka_free_object(&vka, &child_fault_ep);  // Without this, endpoints leak
```
Leaking fault endpoints causes "Insufficient memory" errors after ~15 spawns.

### Allocator Pool Sizing (v0.4.20)
With 94 programs on disk and frequent exec/cleanup cycles, the default allocator pool is too small:
```c
#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 800)  // 3.2MB
```
400 pages was insufficient for large CPIO images. 800 pages handles 94-program workloads.

### Process Exit Must Not Call seL4_DebugHalt (v0.4.28)
The default `sys_exit()` in libsel4muslcsys calls `abort()` which calls `seL4_DebugHalt()` — this kills the ENTIRE system, not just the process. Override exit to trigger a VM fault instead:
```c
static long aios_sys_exit(va_list ap) {
    volatile int *null = (volatile int *)0;
    *null = 0;  // VM fault caught by exec_thread
    __builtin_unreachable();
}
```
exec_thread catches the fault on the child's fault endpoint, cleans up, and returns control to the shell. This means `exit(1)`, `abort()`, and invalid args no longer crash the OS.

### Thread Affinity for SMP
```c
seL4_TCB_SetAffinity(thread.tcb.cptr, core_id);  // 0-3
```
Only available with `CONFIG_ENABLE_SMP_SUPPORT` and non-MCS. Interleaved UART output proves true parallel execution.

## Reply Cap Management (non-MCS)

### The Core Problem
On non-MCS seL4, each `seL4_Recv()` overwrites the thread's implicit reply cap. If a thread receives a Call from client A, then does `seL4_Recv()` on another endpoint (e.g., waiting for a child to exit), the reply cap to A is destroyed. The Reply to A will silently fail and A blocks forever.

### The Solution: SaveCaller
Before doing any Recv that would overwrite the reply cap:
```c
seL4_CNode_Delete(seL4_CapInitThreadCNode, reply_slot, seL4_WordBits);
seL4_CNode_SaveCaller(seL4_CapInitThreadCNode, reply_slot, seL4_WordBits);
// ... do other Recvs ...
seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
```

The slot must be empty before SaveCaller. Delete before each SaveCaller.

### Dedicated Threads for Blocking Services
Any service that needs to block must run in its own thread. Pattern:
- exec_thread: Recv exec request -> SaveCaller -> spawn child -> Recv child fault -> Send reply
- fs_thread: Recv fs request -> do block I/O -> Reply

## virtio-blk Driver

### Device Discovery
QEMU virt has 32 virtio-mmio slots at `0x0a000000`, spaced `0x200` apart. The block device is typically at slot 31. Probe all slots checking `VIRTIO_MMIO_MAGIC == 0x74726976` and `DEVICE_ID == 2`.

### DMA Contiguity via Single Untyped (v0.4.20)
Individual `vka_alloc_frame()` calls do NOT guarantee physically contiguous pages. With large CPIO images or many programs, the allocator fragments and pages are non-contiguous. virtio legacy REQUIRES contiguous physical memory.

Solution: allocate a single 16K untyped and Retype into 4 frames:
```c
vka_object_t dma_ut;
vka_alloc_untyped(&vka, 14, &dma_ut);  // 2^14 = 16K
for (int i = 0; i < 4; i++) {
    seL4_CPtr slot;
    vka_cspace_alloc(&vka, &slot);
    seL4_Untyped_Retype(dma_ut.cptr, seL4_ARM_SmallPageObject,
        seL4_PageBits, seL4_CapInitThreadCNode, 0, 0, slot, 1);
    dma_caps[i] = slot;
}
```
Frames from a single untyped are guaranteed contiguous.

### DMA Layout
Legacy virtio needs specific offsets within the contiguous region:
```
Offset 0x0000: virtq_desc[16]   (256 bytes)
Offset 0x0100: virtq_avail      (38 bytes)
Offset 0x1000: virtq_used       (134 bytes)  <- page boundary
Offset 0x2000: virtio_blk_req   (529 bytes)
```

### Write Support (v0.4.19)
`VIRTIO_BLK_T_OUT` (type=1) for writes. Same descriptor chain as reads but descriptor 1 does NOT have `VIRTQ_DESC_F_WRITE` flag (device reads from host memory, not writes to it).

### Memory Barriers Are Essential
```c
__asm__ volatile("dmb sy" ::: "memory");
```
Without them, the device may not see the updated available ring.

## ext2 Filesystem

### Never Use Struct Packing on AArch64
`__attribute__((packed))` on ext2 structs still fails on AArch64. Always use raw byte reads:
```c
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24); }
```

### Superblock Location
ext2 superblock at offset 1024 (sector 2 for 512-byte sectors). Read sectors 2 and 3 for full 1024-byte superblock.

### Indirect Blocks for Large Files (v0.4.22)
ELF binaries are ~200-400KB. With 1024-byte blocks, direct blocks (12) only cover 12KB. Single indirect adds 256 blocks (256KB). Double indirect adds 65536 blocks (64MB). Both needed for disk-loaded programs.

### Multi-Group Block Allocation (v0.4.29)
With 94 programs at ~200KB each, group 0's 8192 blocks (8MB) fills up. The runtime ext2_alloc_block() must scan all block groups via the BGDT (block group descriptor table), not just group 0:
```c
// Read BGDT at block first_data_block+1
// For each group, read its block bitmap, find free bit
// Return group_start + bit_offset
```

### Directory Entry Splitting
When adding entries to full directories, split the last entry's slack space. Walk entries: each has `rec_len` which may be larger than needed. The difference is slack. Split to insert new entry in the slack.

### Multi-Block Directories (v0.4.29)
With 94+ files in /bin, a single 1024-byte directory block overflows (~30 entries max). The ext2 builder must support multiple data blocks per directory, updating the inode's block pointers and size accordingly.

### Directory Entry Walking
Variable-length records: inode(4) + rec_len(2) + name_len(1) + file_type(1) + name(name_len). Walk with `off += rec_len`. Stop when `rec_len == 0`.

## ELF Loading from Disk (v0.4.22)

### Bypassing CPIO
Default sel4utils loads ELFs from a CPIO archive embedded in the root task. To load from disk:
1. Configure process WITHOUT `process_config_elf()` (no `is_elf` flag)
2. Read ELF into buffer via VFS
3. Call `elf_newFile(buf, size, &elf)` to parse
4. Call `sel4utils_elf_load(&proc.vspace, &vspace, &vka, &vka, &elf)` to map
5. Set `proc.entry_point` and `proc.sysinfo` manually

### ELF Buffer Size (v0.4.26)
Static 1MB buffer. Largest sbase tool is ~377KB (date with libutf). 1MB covers all current tools. For larger programs (gcc, python), need dynamic allocation via vka_alloc_frame pages. Track for v0.5.x.

## POSIX Shim

### __wrap_main Pattern (v0.4.21)
The `--wrap=main` linker flag renames `main` to `__real_main` and our `__wrap_main` intercepts. This is cleaner than constructors because:
- Can strip cap args from argv before real main sees them
- Works with any C program — no source modifications needed
- argv layout: [serial_ep, fs_ep, CWD, progname, arg1, ...]
- __wrap_main extracts caps, sets CWD, calls __real_main(argc-3, argv+3)

### Re-initialization Guard (v0.4.26)
Programs with AIOS_INIT() call aios_init() again after __wrap_main already initialized. Guard:
```c
if (ser_ep && serial_ep == 0) return;  // Already initialized
```

### CWD Propagation (v0.4.29)
Children don't inherit CWD from the shell. Solution:
1. Shell appends `CWD=/path` to exec command string
2. exec_thread extracts CWD marker, passes as argv[2]
3. __wrap_main calls aios_set_cwd() with argv[2]
4. All file operations use resolve_path() for relative paths

### Path Resolution Must Be Everywhere (v0.4.29)
Every file operation must resolve relative paths: open, openat, fstatat, access, chdir, mkdirat, unlinkat. Missing resolution in ANY one causes "No such file" errors. sbase tools use relative paths extensively (ls calls opendir("."), cat opens relative names, etc.).

Central resolver:
```c
static void resolve_path(const char *pathname, char *out, int outsz) {
    if (pathname[0] == '/') { /* absolute - copy directly */ }
    else { /* prepend aios_cwd + / + pathname */ }
}
```
Call resolve_path() BEFORE every fetch_file(), fetch_dir_as_getdents(), fetch_stat().

### aarch64 Syscall Numbers (v0.4.27)
aarch64 uses `__NR_fstatat` (79), NOT `__NR_newfstatat`. The `#ifdef __NR_newfstatat` guard fails silently — fstatat never gets registered, and sbase ls crashes with "Function not implemented". Always verify syscall numbers against the actual header:
```bash
grep "__NR_fstatat" build-04/projects/musllibc/build-temp/stage/include/bits/syscall.h
```

### muslcsys Syscall Table
`MUSLC_HIGHEST_SYSCALL=452` so all numbers fit. Use `muslcsys_install_syscall()` to override at runtime. The init_syscall_table constructor runs at CONSTRUCTOR_MIN_PRIORITY (101).

### Forward Declarations Matter
Functions used across the file (like fetch_stat, resolve_path) must be declared before first use. C compilers process top-to-bottom. If aios_sys_open uses fetch_stat but fetch_stat is defined 200 lines later, add a forward declaration near the top.

### Variable Scope in Large Functions
Static variables like `aios_cwd` must be declared before any function that references them. resolve_path() at line 40 can't see aios_cwd declared at line 480.

### Environment Variables (v0.4.25)
Set `extern char **environ` to point at a static env array in aios_init(). This makes getenv() work for all C programs. Shell has its own env table and passes CWD via exec args.

## Cross-Compilation (aios-cc)

### Object File Collisions (v0.4.29)
When compiling multiple files with the same basename (e.g., `sbase/cp.c` and `sbase/libutil/cp.c`), both produce `/tmp/aios_cp.o` — the second overwrites the first, losing `main()`. Fix: use `tr "/" "_"` to create unique object names from full paths.

### Force-Linking aios_posix
Programs that don't reference aios_posix symbols directly won't pull in the library from a static archive. Use `--wrap=main` which creates a reference that forces linkage.

### CRT Objects
Link order matters:
```
crt0.o crti.o crtbegin.o [objects] [libraries] crtend.o crtn.o
```
Missing crtn.o causes constructors/destructors to not run. Duplicate crtend.o causes "multiple definition of __TMC_END__".

## IPC Patterns

### Packing Strings in Message Registers
seL4 MRs are 64-bit words. Pack 8 ASCII characters per MR. Maximum ~960 bytes. For ELF data, use VFS read into buffer instead.

### IPC Labels as Operation Codes
Convention: message label identifies the operation, MR0..MRn carry data.
- Serial: 1=PUTC, 2=GETC, 3=PUTS, 4=KEY_PUSH
- FS: 10=LS, 11=CAT, 12=STAT, 14=MKDIR, 15=WRITE_FILE, 16=UNLINK, 17=UNAME
- Exec: 20=RUN

## Build System

### CPIO Caching
Ninja doesn't detect when child ELF binaries change inside CPIO. When changing child apps, do a full rebuild: `rm -rf build-04 && mkdir build-04`.

### GCC 15 + musllibc
GCC 15 linker rejects copy relocations against protected visibility symbols. Patch musllibc before building. See `docs/patches/musl-gcc15.md`.

### GNU sed vs BSD sed (v0.4.21)
After installing GNU sed via Homebrew, bump scripts using `sed -i ''` (BSD style) break. GNU sed uses `sed -i` (no empty string). Use GNU sed syntax since it's first in PATH.

### macOS Shell Gotchas
- zsh interprets `#` as comment on command lines — avoid in heredocs
- Single-quote escaping in heredocs is extremely fragile
- Python scripts are ALWAYS safer than sed/heredocs for code modifications
- `ls $VAR/*.c | tr '\n' ' '` doesn't expand the same as `$VAR/*.c` glob
- Always use glob expansion directly in command, not via variable substitution

### Symlinks Required
seL4's CMake build expects specific directory structure. If `tools/seL4/cmake-tool` gets cleaned up, rebuild breaks silently.

### GNU Coreutils Bootstrap
Requires: autoconf, automake, gperf, texinfo (makeinfo), wget. Even after bootstrap, cross-compilation fails due to gnulib/C23 header issues (stdcountof.h). Use sbase instead.

## UART I/O

### TX via seL4_DebugPutChar
Direct PL011 TX writes conflict with kernel debug output. Use `seL4_DebugPutChar()` for all output (requires `KernelDebugBuild=ON`).

### Suppressing Boot Warning (v0.4.14)
libsel4platsupport prints "Warning: using printf before serial is set up" on every early printf. Comment out in `deps/seL4_libs/libsel4platsupport/src/common.c` line 275. Documented in `docs/patches/platsupport-warning.md`.

### RX via Polling
Root task polls `UART_FR` register for RXFE flag, reads `UART_DR`. No interrupt-driven input yet.

## Debugging Tips

### Silent Freezes
Almost always a priority issue. If the system freezes after spawning processes, check that all cooperating threads/processes are at equal priority.

### "Destination slot not empty"
The CSpace slot already contains a capability. Delete first with `seL4_CNode_Delete()`.

### "Failed to allocate cslot"
Root CNode is full. Increase `KernelRootCNodeSizeBits`.

### "Insufficient memory" During Exec
Either fault endpoints are leaking (check vka_free_object after destroy_process) or allocator pool is too small (increase to 800+ pages).

### "Function not implemented" from sbase tools
Missing syscall override. Check the syscall number in `bits/syscall.h`, verify it's registered in aios_init(), and check the `#ifdef` guard matches the actual define name for aarch64.

### System Dies on Program Error
Process called exit() or abort() which invoked seL4_DebugHalt(). Override sys_exit and sys_exit_group to trigger VM fault instead.

### Fault Messages (debug build)
```
vm fault on data at address 0x0 with status 0x93800007
```
Use `aarch64-linux-gnu-addr2line` to find the source line.

### ls Shows Root Contents in Subdirectory
CWD not propagated to child process. Check: shell sends CWD= marker, exec_thread extracts it, __wrap_main sets it, resolve_path uses it.

## Disk Image Building

### ext2 Builder Architecture (v0.4.29)
Modular Python builder in `scripts/ext2/builder.py`. Supports:
- Multi-group block allocation (16 groups for 128MB)
- Multi-block directories (for /bin with 94+ entries)
- Indirect blocks (single + double) for large ELFs
- --rootfs overlay from disk/rootfs/ directory
- --install-elfs from multiple directories (sbase + AIOS)

### Disk Size Planning
94 programs at ~200-400KB each = ~22MB of ELFs. With filesystem overhead, 128MB is comfortable. The builder uses ~35K blocks across 16 groups.

### Content Should Not Be Hardcoded
Early versions hardcoded /etc/motd, /etc/passwd etc. in the Python builder. Now all content lives in `disk/rootfs/` and gets installed via --rootfs flag. Builder only creates the directory skeleton (bin, sbin, etc, home, tmp, dev, var, proc).

## Cross-VSpace Thread Creation (v0.4.30)

### seL4_TCB_Configure Has Mixed Cap Resolution
Parameters `cspace_root`, `vspace_root`, and `bufferFrame` are resolved from the **caller's** CSpace at configure time. But `fault_ep` is a raw CPtr **stored in the TCB** and resolved from the **thread's own CSpace** at fault time. Passing a root-side slot number for fault_ep causes a cap fault when the thread faults, because the child's 12-bit CSpace doesn't contain that slot.

Solution: copy the fault endpoint into the child's CSpace via `sel4utils_copy_cap_to_process`, pass the child-side slot to TCB_Configure, and Recv on the root-side cap.

### Manual TCB Creation for Child Process Threads
`sel4utils_configure_thread` cannot create threads in another process's VSpace because it passes the IPC buffer cap from root's CSpace, but the kernel resolves it from root's CSpace (caller). This happens to work. However, fault_ep does NOT work the same way. Full manual setup required:
1. `vka_alloc_tcb` — allocate TCB
2. `vka_alloc_endpoint` — fault EP
3. `vka_alloc_frame` — IPC buffer
4. `vspace_map_pages(&child_vspace, ...)` — map IPC buffer + stack into child
5. `sel4utils_copy_cap_to_process` — copy fault EP to child CSpace
6. `seL4_TCB_Configure(tcb, child_fault_cap, child_cnode, guard, child_pgd, ...)` 
7. `seL4_TCB_SetPriority` + `seL4_TCB_WriteRegisters` with resume=1

### Mutex Without IPC
Threads sharing a VSpace can use `__atomic_test_and_set`/`__atomic_clear` for spinlock mutexes. `seL4_Yield()` in the spin loop is essential — at equal priority, without it the spinning thread never yields and other threads starve.

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
// Clear slot (must be empty)
seL4_CNode_Delete(seL4_CapInitThreadCNode, reply_slot, seL4_WordBits);
// Save reply cap from caller
seL4_CNode_SaveCaller(seL4_CapInitThreadCNode, reply_slot, seL4_WordBits);

// ... do other Recvs (e.g., wait for child fault) ...

// Reply to original caller via saved cap
seL4_SetMR(0, result);
seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
```

The slot must be empty before SaveCaller. An endpoint object in the slot causes "Destination slot not empty" error. Allocate a slot with `vka_cspace_alloc_path()`, not `vka_alloc_endpoint()`. Delete before each SaveCaller.

### Dedicated Threads for Blocking Services
Any service that needs to block (exec waits for child, fs waits for disk) must run in its own thread. The keyboard polling loop in root must never block. Pattern:
- exec_thread: Recv exec request → SaveCaller → spawn child → Recv child fault → Send reply
- fs_thread: Recv fs request → do block I/O → Reply
- root main loop: Poll UART RX → forward KEY_PUSH via IPC → Yield

## virtio-blk Driver

### Device Discovery
QEMU virt has 32 virtio-mmio slots at `0x0a000000`, spaced `0x200` apart (16 KB total = 4 pages). The block device is typically at slot 31 (`0x0a003e00`). Probe all slots checking `VIRTIO_MMIO_MAGIC == 0x74726976` and `DEVICE_ID == 2`.

### MMIO Mapping
Must map 4 pages to cover all 32 slots. A single page only covers slots 0-7.

### DMA Setup
Legacy virtio (v1) needs the virtqueue descriptor table, available ring, and used ring at specific offsets within a physically contiguous region:
```
Offset 0x0000: virtq_desc[16]   (256 bytes)
Offset 0x0100: virtq_avail      (38 bytes)
Offset 0x1000: virtq_used       (134 bytes)  ← MUST be at page boundary
Offset 0x2000: virtio_blk_req   (529 bytes)
```

The used ring at offset `0x1000` is critical — legacy virtio aligns it to page boundary. A single 4K page for all three structures will fail. Allocate 4 contiguous pages (16K).

### Memory Barriers Are Essential
```c
__asm__ volatile("dmb sy" ::: "memory");
```
Without them, the device may not see the updated available ring.

## ext2 Filesystem

### Never Use Struct Packing on AArch64
`__attribute__((packed))` on ext2 structs still fails on AArch64. The compiler's struct layout doesn't match the on-disk format. Always use raw byte reads:
```c
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24); }
```
Verified: packed struct read `s_magic` as `0x0001` while raw bytes correctly read `0xEF53`.

### Superblock Location
ext2 superblock at offset 1024 (sector 2 for 512-byte sectors). Read sectors 2 and 3 for full 1024-byte superblock.

### Directory Entry Walking
Variable-length records: inode(4) + rec_len(2) + name_len(1) + file_type(1) + name(name_len). Walk with `off += rec_len`. Stop when `rec_len == 0`.

## IPC Patterns

### Packing Strings in Message Registers
seL4 MRs are 64-bit words. Pack 8 ASCII characters per MR. Maximum message size is `seL4_MsgMaxLength` MRs (~120 words = ~960 bytes). For larger data, need shared memory.

### IPC Labels as Operation Codes
Convention: message label identifies the operation, MR0..MRn carry data.
- Serial: 1=PUTC, 2=GETC, 3=PUTS, 4=KEY_PUSH
- FS: 10=LS, 11=CAT
- Exec: 20=RUN

## Build System

### CPIO Caching
Ninja doesn't detect when child ELF binaries change inside CPIO. When changing child apps, do a full rebuild: `rm -rf build-04 && mkdir build-04`.

### GCC 15 + musllibc
GCC 15 linker rejects copy relocations against protected visibility symbols. Patch musllibc before building. See `docs/patches/musl-gcc15.md`.

### macOS Gotchas
- GNU cpio required: `brew install cpio`, add to PATH before system cpio
- zsh interprets `#` in heredocs — avoid shell comments in heredocs
- Python scripts are safer than complex heredocs for code generation

### Symlinks Required
seL4's CMake build expects specific directory structure. If `tools/seL4/cmake-tool` gets cleaned up, rebuild breaks silently with "include failed".

## UART I/O

### TX via seL4_DebugPutChar
Direct PL011 TX writes conflict with kernel debug output. Use `seL4_DebugPutChar()` for all output (requires `KernelDebugBuild=ON`).

### RX via Polling
Root task polls `UART_FR` register for RXFE flag, reads `UART_DR`. No interrupt-driven input yet.

## Debugging Tips

### Silent Freezes
Almost always a priority issue. If the system freezes after spawning processes, check that all cooperating threads/processes are at equal priority.

### "Destination slot not empty"
The CSpace slot already contains a capability. Delete first with `seL4_CNode_Delete()` or allocate a fresh empty slot.

### "Failed to allocate cslot"
Root CNode is full. Increase `KernelRootCNodeSizeBits` in settings.cmake and full rebuild.

### Fault Messages (debug build)
```
vm fault on data at address 0x0 with status 0x93800007
in thread 0xffffff... "rootserver" at address 0x4031dc
```
Use `aarch64-linux-gnu-addr2line` to find the source line.

# BL-2 de-risk: how seL4-coupled is AIOS userspace? (2026-06-23, s14)

Codebase-grounded measurement (analysis workflow + independent spot-check). Answers the COST axis of
the seL4->Linux pivot: is porting AIOS userspace to a Linux base cheap or expensive? It does NOT decide
the pivot (that is the thesis question -- see end).

## The number (concentration, not pervasiveness)
- AIOS userspace ~51K LoC. **~53% of files have ZERO seL4 references** -- filesystems (fat32.c, ext2.c),
  the TCP/DHCP stack (net_tcp.c, net_dhcp.c), crypto, SSH/SFTP (ssh_kex.c), GPU/v3d, boot config/DTB
  parsing, and most apps. Independently spot-checked: fat32.c / net_tcp.c / ssh_kex.c = 0 seL4 refs.
- seL4 coupling is **CONCENTRATED**: the top ~10 files (the servers + boot + the shim) hold ~57% of all
  seL4 reference lines. Heaviest: pipe_server.c, net_server.c, fs_server.c, exec_server.c, fork.c,
  aios_root.c. That is a localized seam, not coupling smeared across every file.

## The seam (the load-bearing finding -- independently verified)
AIOS has a **POSIX shim**: `src/lib/aios_posix.h` + `src/lib/posix_*.c` (10 files, ~4,154 lines) that
maps POSIX calls (open/read/write/fork/execve/socket) onto seL4 IPC to the core-0 servers. Verified:
only **~406 of 4,154 shim lines (~10%) touch seL4** -- the rest is POSIX logic. Apps call POSIX; seL4
is invisible above the shim. So the seam IS clean: porting = reimplement the shim backend on Linux
syscalls, and the apps (dash, sshd, the sbase tools, mini_shell) come along largely unchanged.

## Port cost (honest, with the agent estimate tempered)
The analysis estimates **~8-12 person-weeks** (one senior eng; MVP shell+FS+SSH in ~4-6). Treat that as
a FLOOR -- port estimates run optimistic. The cheap parts are real (delete fork.c/cow.c and use Linux
fork+kernel CoW; the zero-coupling fs/net/crypto/ssh recompile as-is). The under-counted risks are
SEMANTIC, not LoC:
- **Server IPC-shape change:** every server's main loop is seL4 `Recv/ReplyRecv`; on Linux it becomes
  sockets/epoll/futex. Business logic (ring buffers, TCP state machine) is portable, but the loop + the
  zero-copy SHM-ring (pipe_server) is real rework.
- **The capability/auth model:** AIOS's badge-based identity + privesc gating maps to Linux uids/caps/
  seccomp -- a security-semantics translation, not a line count. You GAIN ASLR/DEP/seccomp, you LOSE
  capability-based isolation (this is the thesis trade, not a cost).
- **Driver/IRQ glue:** seL4 IRQ-cap binding + DMA frame alloc -> Linux request_irq/dma_alloc_coherent
  (per-driver ~30-40 lines; logic reused).
Realistic: **a few months for a senior eng to an MVP, bounded engineering, no unknown-unknowns** -- the
seam being clean is exactly why.

## Cheapest validating prototype (do this FIRST -- days)
**mini_shell on Linux x86-64.** Reimplement `posix_file.c` + `posix_proc.c` (~280 LoC) as Linux syscall
wrappers, recompile `src/apps/mini_shell.c` against a Linux libc, run on x86-64 QEMU; success = `ls`,
`cat`, `mkdir`, pipes (`cmd | cmd`), redirects all work with NO server present (use the Linux FS
directly). If the shim+apps run unchanged, the whole port thesis holds; if it fights, that is decisive
early evidence. This is the BL-2 deliverable's concrete next step.

## Verdict
**COST is de-risked: the port is bounded engineering (~months, clean seam), not a research project --
precisely because AIOS already has a well-drawn POSIX seam.** That removes cost as a blocker to the
pivot. (The analysis agent went further to "GO, PIVOT"; that overreaches -- cheap-to-do is not
should-do.) The decision still reduces to the THESIS QUESTION: is seL4's capability isolation /
verification the point of AIOS? If the novelty lives ABOVE the kernel, the cheap clean port is a green
light to Linux-native (x86-64 + driver tree for free). If the novelty IS the microkernel isolation, the
port cheaply preserves the code but discards exactly the architecture that mattered. BL-2 says you CAN;
the thesis says whether you SHOULD. Ref: BACKLOG BL-1/BL-2, docs/DR_20260623_linux_driver_reuse_on_sel4.md.

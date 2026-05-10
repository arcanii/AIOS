# seL4 -- AIOS Developer Investigation

A briefing for a fresh session looking at the microkernel side of AIOS:
what version we run, what we already touch, what's been painful, and
what diagnostic work would pay off without compromising the
verification story.

Read this end-to-end first, then HANDOVER.md for the current AIOS
state. The repo root is `~/Desktop/github_repos/AIOS`.

---

## Snapshot

* **Kernel**: seL4 15.0.0-dev (kernel/VERSION → `3f590a602 Update VERSION
  file to 15.0.0-dev`). In-tree at `kernel/`, not a submodule -- we can
  edit freely.
* **Build options** (see `settings.cmake` and `settings-rpi4.cmake`):
  * `KernelDebugBuild=ON` -- `seL4_Debug*` ABI available
  * `KernelPrinting=ON` -- `printf()` from kernel works
  * `KernelVerificationBuild=OFF` -- we can patch the kernel without
    breaking a verification build (we're never going to ship a
    verified AIOS)
  * `KernelIsMCS=OFF` -- non-MCS, so `seL4_Call` has no timeout and
    fault replies use the legacy reply-cap-in-TCB pattern
  * `KernelMaxNumNodes=4` on both platforms as of v0.4.134
  * `KernelArmHypervisorSupport=ON` (qemu-virt) / `OFF` (rpi4)
* **Platforms**:
  * `PLAT_QEMU_VIRT` -- `qemu-system-aarch64 -M virt`, our main dev
    target. Works on all CPUs we care about, SMP runs.
  * `PLAT_RPI4` -- `bcm2711` / cortex-a72. Recently re-enabled SMP
    (v0.4.134); first hardware boot is the focus of
    `hw/rpi4/HARDWARE_TEST.md`.

### How AIOS uses kernel debug calls today

```
src/aios_root.c        seL4_DebugHalt()        // PSCI-equivalent shutdown
src/apps/serial_server.c   seL4_DebugPutChar() // legacy fallback path
src/apps/tty_server.c      seL4_DebugPutChar() // current TTY output
```

That's it. We do **not** currently use:

* `seL4_DebugDumpScheduler` -- prints the scheduler state, helpful
  when something's pinned or starved
* `seL4_DebugSnapshot` -- full kernel state dump
* `seL4_DebugCapIdentify(cap)` -- returns the cap type for a cptr, the
  most useful "what is this cap actually?" tool when debugging cspace
  layouts
* `seL4_DebugNameThread(tcb, "name")` -- attaches a name string the
  kernel uses in fault prints (`"child of: 'rootserver'"` becomes the
  name string)
* `seL4_DebugSendIPI` -- send a manual IPI to a specific core; useful
  for SMP wedge debugging
* `seL4_DebugRun(fn, arg)` -- run a callback in kernel context. Very
  blunt, but lets you read any kernel memory address without rebuilding

---

## Cases where better kernel diagnostics would have saved us hours

These are concrete, not hypothetical. Each one cost at least half a
session of guesswork; better visibility from the kernel side would
have cut the diagnostic loop.

### 1. COW Phase 2 attempt -- "Invocation of invalid cap" in forked child

From `docs/NEXT_20260502b.md`:

```
seL4 ... Invocation of invalid cap #18446744073709551615
Caught cap fault in send phase at address 0
while trying to handle: cap fault in send phase at address 0xffffffffffffffff
in thread "child of: 'rootserver'" at address 0x46233c (arm_sys_send_recv)
```

The cptr is `(seL4_Word)-1` -- uninitialised memory. The thread name
is `"child of: 'rootserver'"` which is whatever the parent named
itself. From this we learned nothing about *which* cap, *which*
syscall, or *which* cspace slot. We had to revert the entire
session's work to bisect.

**What would have helped**:
* In the cap-fault print path, dump the lookup_fault details
  (`invalid_root`, `missing_capability`, `depth_mismatch`,
  `guard_mismatch`) -- the kernel already has these in
  `current_lookup_fault` but only the simple "cap fault in send
  phase" string makes it to the console.
* When the cap is `(seL4_Word)-1`, log a hint that this is almost
  certainly an uninitialised variable rather than a deleted cap.
* On send-phase cap fault, log the syscall label and the current PC
  region (we have the PC in `tptr->tcbContext`).

### 2. COW Step 3 wc/shutdown EPERM (still open)

From `docs/NEXT_20260503a.md`: dash post-promotion can't fork+exec
again. We suspect cap allocation or cspace copy gets confused by the
orphaned parent_cap, but we don't know which path returns -1.

**What would help**:
* Per-syscall optional trace: when this thread does any cap operation,
  log the operation + result. Filter to a single TCB by name.
* `seL4_CNode_Copy` failure path -- the kernel knows exactly why
  (e.g. derived cap precludes copy) but the user-space site sees only
  the errno-equivalent return.

### 3. SMP secondary cores on RPi4 (untested)

v0.4.134 enables `KernelMaxNumNodes=4` on RPi4. If a core fails to
come up, the diagnostic is currently "fewer `Core N is up` lines
appear in the elfloader log." We don't know whether:
* the elfloader's spin-table write reached the right address
* the secondary CPU saw the write
* the secondary CPU ran our entry and crashed somewhere

**What would help**:
* Per-core boot trace points written to a fixed memory address that
  the boot core can dump after timeout. Minimal cost, very high
  signal.
* Optional: `seL4_DebugSendIPI` from user space, to confirm the
  secondary is alive at all.

### 4. VKA pool exhaustion silence

We've had `vka_alloc_object_at_maybe_dev@object.h:57 Failed to
allocate object of size 32, error 1` show up in the smoke logs. This
is libsel4allocman, not the kernel, but the underlying retype that
failed has no diagnostic. We had to count pages by hand to figure out
the budget.

**What would help**:
* On `Retype` failure, kernel logs (under `KernelDebugBuild`): which
  UT cap, what was requested, what's actually free in that UT.

### 5. "Range for vaddr X not reserved" downstream noise

These come from libsel4utils' `vspace_new_pages_at_vaddr`, not the
kernel. But they correlate with kernel-level page-table state. Knowing
*from the kernel side* whether a PTE was already populated at the
faulting va would unambiguously say "this is a stale tracking entry"
vs "this is genuinely unmapped."

**What would help**:
* Cheap kernel ABI: `seL4_DebugDumpPTE(vspace_root, va)` returns the
  PTE bits + paddr. Tells us instantly whether the kernel thinks
  there's something there.

### 6. Schedule / runqueue inspection on SMP

We hit at least two cases where threads seemed to be starved:
serverstats probe at priority 180 (v0.4.121 first attempt), and the
documented "no qemu busy-poll" feedback for net_server. In both, the
diagnosis was "the thread isn't running" with no kernel-side
confirmation.

**What would help**:
* Use `seL4_DebugDumpScheduler` from a debug `/proc` file. Already
  exists in kernel ABI; just needs a user-space wrapper.

---

## Existing seL4 debug ABI -- a closer look

These are the calls already available with `KernelDebugBuild=ON`. We
should use them more before adding new ones.

| call | what it does | first use case for AIOS |
|------|--------------|--------------------------|
| `seL4_DebugDumpScheduler()` | prints per-core ready queue and current thread | new `/proc/scheduler` |
| `seL4_DebugCapIdentify(cap)` | returns cap type as uint32 | could replace ad-hoc cap probing in cow.c |
| `seL4_DebugNameThread(tcb, name)` | attaches a thread name kernel uses in fault prints | name every server thread + process so faults are legible |
| `seL4_DebugSnapshot()` | full state dump | last-ditch in panic handler |
| `seL4_DebugSendIPI(core, irq)` | send a manual IPI | SMP wedge debugging |
| `seL4_DebugRun(fn, arg)` | run callback in kernel context | dangerous; only behind an explicit `/proc/kdebug` knob |
| `seL4_DebugHalt()` | halt the kernel (qemu: exits, real hw: hangs) | we already use this for shutdown |
| `seL4_DebugPutChar(c)` | direct kernel UART write | already used for early TTY |

**Low-hanging fruit**: name every long-lived thread. Right now fault
prints say `"child of: 'rootserver'"` for everything because no
`seL4_DebugNameThread` calls exist. After naming, faults read
`"child of: 'pipe_server'"` or `"child of: 'dash'"`, which is the
single biggest legibility win for the cost.

---

## How to safely patch the kernel for AIOS-specific diagnostics

Ground rules so we don't accumulate technical debt or fork the kernel:

1. **All AIOS-specific kernel code lives behind a single gate.** Add to
   `kernel/Kconfig`:

   ```
   config AIOS_KDEBUG
       bool "AIOS extra developer diagnostics"
       default n
       depends on PRINTING && DEBUG_BUILD
       help
         Enables AIOS-specific kernel diagnostics that are too
         verbose or invasive for upstream seL4 debug builds.
   ```

   Then in `settings.cmake` / `settings-rpi4.cmake`:

   ```cmake
   set(KernelAIOSKDebug ON CACHE BOOL "" FORCE)
   ```

   In kernel source, gate with `#ifdef CONFIG_AIOS_KDEBUG`. A single
   `git grep CONFIG_AIOS_KDEBUG kernel/` shows every patch we've
   applied -- trivial to audit and revert.

2. **Never modify the verified API contract.** Don't change function
   signatures, return-value semantics, or invariants. Adding a
   `printf` next to an existing one is fine; changing what
   `seL4_CNode_Copy` returns is not.

3. **Prefer info dumps over behaviour changes.** A patch that prints
   more on an existing failure path is cheap and reviewable. A patch
   that changes scheduling or capability-tree semantics is forking
   the kernel.

4. **Keep patches small and self-contained.** Aim for <50 LOC per
   patch, one file each. If a diagnostic needs cross-file changes,
   it's probably also a feature change in disguise.

5. **Document each patch in this file.** Section "Applied patches"
   below; future-you wants to know what's there without grepping.

---

## Concrete first-session investigations

Ordered by value-per-hour, low-risk first.

### A. Name all long-lived threads (~1 hour, no kernel patch)

`seL4_DebugNameThread(tcb, "name")` is a user-space call. Sites to
add:

* `src/boot/boot_services.c` after each `start_server_thread` --
  name pipe_server, fs_server, exec_server, thread_server,
  net_server, display_server, crypto_server, serverstats.
* `src/process/fork.c` after child TCB creation -- name with the
  child's pid or program name.
* `src/servers/exec_server.c` after spawning getty / new procs --
  name with the program basename.

After this, fault prints become legible:
```
in thread 0x... "dash@pid7" at address 0x...
```
instead of `"child of: 'rootserver'"`.

Verify under qemu-virt by intentionally crashing a known process
(e.g. `kill -SEGV $$` in dash). The fault print should show "dash".

### B. Verbose cap-fault dump (~30 LOC kernel patch, behind AIOS_KDEBUG)

In `kernel/src/kernel/faulthandler.c:print_fault`, on
`seL4_Fault_CapFault`:

```c
#ifdef CONFIG_AIOS_KDEBUG
    /* AIOS: dump lookup_fault details */
    lookup_fault_t lf = current_lookup_fault;
    switch (lookup_fault_get_lufType(lf)) {
    case lookup_fault_invalid_root:
        printf(" [invalid root]"); break;
    case lookup_fault_missing_capability:
        printf(" [missing cap at depth %lu]",
               lookup_fault_missing_capability_get_bitsLeft(lf));
        break;
    case lookup_fault_depth_mismatch:
        printf(" [depth mismatch %lu vs %lu]",
               lookup_fault_depth_mismatch_get_bitsLeft(lf),
               lookup_fault_depth_mismatch_get_bitsFound(lf));
        break;
    case lookup_fault_guard_mismatch:
        printf(" [guard mismatch g=%lx g_bits=%lu f_bits=%lu]",
               lookup_fault_guard_mismatch_get_guardFound(lf),
               lookup_fault_guard_mismatch_get_bitsLeft(lf),
               lookup_fault_guard_mismatch_get_bitsFound(lf));
        break;
    }
    if (cptr == (seL4_Word)-1) {
        printf(" [HINT: cptr=-1 is almost always uninitialised memory]");
    }
#endif
```

Validates against the COW Phase 2 cap-fault repro (re-enable
`COW_STRIP_PARENT=1` in cow.c and let dash crash second-fork).

### C. `/proc/scheduler` user-space view (~50 LOC, no kernel patch)

Add a `/proc/scheduler` reader that calls `seL4_DebugDumpScheduler()`
and captures the kernel printf output. Tricky bit: kernel printf goes
to UART, not back to the caller. Easiest path: redirect kernel
printf to a ring buffer (already done for kernel log in
`aios_log.c`?) and read the ring's tail since the call.

If that turns out to be more than 50 LOC, drop it -- the call is
available directly from any C code and the operator can invoke it
from a test app instead of a `/proc` file.

### D. `seL4_DebugDumpPTE(vspace_root, va)` (~80 LOC kernel patch, behind AIOS_KDEBUG)

Adds a new debug invocation. Walks the page tables for the given
vspace root and va, returns the PTE bits + paddr (or "not mapped").
Exposed via `/proc/cow` extension (`/proc/cow/<pid>/<va>`?) or a
test app.

This is the most valuable for the "Range for vaddr X not reserved"
class of bugs because it answers the user-space question "what does
the kernel think is at this va?" definitively.

Skeleton:

```c
#ifdef CONFIG_AIOS_KDEBUG
exception_t decodeAIOSDumpPTE(cap_t cap, word_t length, word_t *buffer)
{
    /* assume cap is a vspace root, lookup ptSlot, dump pte */
}
#endif
```

Wire as a new label in `kernel/include/sel4/arch_invocation.h` (a
new `seL4_ARM_VSpace_DumpPTE`) and a thin libsel4 wrapper.

### E. Per-core boot trace for SMP RPi4 (~60 LOC, in elfloader not kernel)

Reserve a fixed physical address (e.g. `0x7000000` -- above kernel,
below DTB). Each secondary core writes its own progress tag on each
boot stage:

```c
/* secondary_startup() */
*((volatile uint32_t *)0x7000000)[cpu_id] = 1; /* entered */
/* after mmu enable */
*((volatile uint32_t *)0x7000000)[cpu_id] = 2; /* mmu */
/* before kernel jump */
*((volatile uint32_t *)0x7000000)[cpu_id] = 3; /* kernel */
```

Boot core, after secondaries-up timeout, prints the array. Tells us
exactly where each core got stuck.

---

## Applied patches

(empty; populate as we land them)

```
CONFIG_AIOS_KDEBUG: not yet wired.

Files modified by AIOS in kernel/:
  (none currently)
```

Every entry should record: file, function, ~LOC, what diagnostic it
adds, how to verify it works, how to revert (`#undef CONFIG_AIOS_KDEBUG`
in settings.cmake should be a clean kill).

---

## Build / rebuild after kernel patches

```
cd build-04 && ninja          # qemu-virt
cd build-rpi4 && ninja        # rpi4
```

Both build trees include kernel/ at full source. There is no separate
"build kernel" step.

If `CONFIG_AIOS_KDEBUG` is added to Kconfig, expect `cmake` reconfig
the first time (`KernelAIOSKDebug` cache var will be new). Run:

```
cd build-04 && cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- .. && ninja
```

---

## What to read before patching anything

| topic | start here |
|-------|-----------|
| fault flow | `kernel/src/kernel/faulthandler.c` (`handleFault`, `print_fault`) |
| VM fault | `kernel/src/arch/arm/64/kernel/vspace.c` (`handleVMFault`, `decodeARMFrameInvocation`) |
| Retype | `kernel/src/object/objecttype.c` (`decodeUntypedInvocation` and `invokeUntyped_Retype`) |
| CDT | `kernel/src/object/cnode.c` (CDT walks, derivation logic) |
| SMP boot | `kernel/src/smp/ipi.c`, `kernel/src/arch/arm/smp/ipi.c`, plus the elfloader spin-table driver at `deps/seL4_tools/elfloader-tool/src/arch-arm/drivers/smp-spin-table.c` |
| schedule | `kernel/src/kernel/thread.c` (`schedule`, `chooseThread`) |
| debug ABI defs | `kernel/libsel4/include/sel4/syscalls.h` + `sel4_arch_include/aarch64/sel4/sel4_arch/syscalls.h` |

The kernel uses generated headers heavily (everything in
`build-04/libsel4/include/sel4/`) -- read the generators if you don't
recognise a function from grep.

---

## What we are NOT doing

* No userland verification effort -- AIOS is research, not formally
  verified, and the kernel-side patches we add explicitly break the
  verification story by gating on a config that's off in the verified
  build.
* No MCS migration. Our COW Step 3 work would benefit from
  call-with-timeout, but switching is a big architectural change
  affecting every server.
* No upstreaming. Patches here are for AIOS development convenience;
  if any look generally useful we file them as separate suggestions
  to seL4 upstream, not as PRs from this tree.

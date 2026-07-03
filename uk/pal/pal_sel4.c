/*
 * pal_sel4.c -- a THIRD PAL backend: the seL4 SEAM-PROVING SCAFFOLD (milestone M6).
 *
 * ============================================================================================
 * WHAT THIS IS (and is NOT)
 * ============================================================================================
 * This file is NOT a working seL4 port. It is a *scaffold* that implements the full include/pal.h
 * contract as documented stubs, one per primitive, each stating the seL4 PROOF OBLIGATION a real
 * port must discharge. It exists to make a single, load-bearing claim TRUE and CHECKABLE:
 *
 *     the AIOS userspace kernel (kernel/aios_kernel.c) is genuinely HOST-AGNOSTIC.
 *
 * `make PAL=sel4` compiles kernel/aios_kernel.c and this file and LINKS them into `aios-uk`
 * WITHOUT pal_linux_common.c and without any Linux-PAL symbol. If the kernel had leaked even one
 * hidden dependency on the Linux backend -- a stray ptrace assumption, a symbol reached around the
 * seam -- the link would FAIL. It links. So the seam in pal.h is complete: the kernel reaches the
 * host ONLY through pal.h, and a non-Linux backend satisfies it. That is the portability proof the
 * seccomp backend (M9) rehearsed with a SECOND Linux trap mechanism; this is the same proof carried
 * to a THIRD, NON-Linux backend -- the one that matters for the endgame (verified seL4 on x86-64;
 * "verification is the soul").
 *
 * Unlike pal_linux.c / pal_seccomp.c -- two trap FRONT-ENDS that share the host-driver + ptrace
 * INJECTOR core in pal_linux_common.c -- this backend is SELF-CONTAINED: it does NOT #include
 * pal_linux_common.c (there is no Linux under seL4). The Makefile encodes that: for PAL=sel4,
 * PAL_COMMON is empty.
 *
 * ============================================================================================
 * WHY A SCAFFOLD, NOT A REAL PORT (the scope, settled with Bryan)
 * ============================================================================================
 * A real seL4 port is a multi-session epic gated on infrastructure not yet stood up: an seL4 SDK,
 * a target (the memory names x86-64; see the design doc's honest note on seL4's arch-specific
 * verification story), and -- because seL4 has NO ptrace and no host kernel -- a trap model built
 * from seL4 primitives (a fault handler or a VMM) plus a root task that constructs each guest's
 * VSpace/TCB/CSpace from capabilities. The scaffold is the bounded, in-session, honest deliverable:
 * it proves the seam and becomes the proof-obligation document. The cross-cutting design (trap
 * model, memory/cap model, the userspace-server story for the host driver, confinement as
 * capability confinement, and the infra a real port needs) is written up in
 *     docs/DESIGN_20260703_pal_sel4_seam.md.
 * Each stub below states its LOCAL obligation; the doc states the GLOBAL ones.
 *
 * ============================================================================================
 * THE ONE STRUCTURAL LESSON seL4 vs. Linux (read this before the stubs)
 * ============================================================================================
 * On Linux, the AIOS kernel is a tracer: it traps a guest's syscalls with ptrace and reaches
 * through the host kernel (process_vm_readv, injected mmap/clone/execve, PTRACE_*REGSET). seL4
 * offers NONE of that. Instead a real pal_sel4.c is a seL4 ROOT TASK holding capabilities, and the
 * pal.h primitives map to seL4 object invocations, in three families:
 *
 *   (A) Guest world  -- a guest is a seL4 THREAD (a TCB) in its own VSpace. Its AIOS-ABI `svc`
 *       traps not by ptrace but by FAULTING to the AIOS kernel's fault endpoint (an unknown-syscall
 *       fault): pal_guest_next == seL4_Recv on that endpoint; pal_guest_return == seL4_Reply with
 *       the result registers; registers via seL4_TCB_ReadRegisters/WriteRegisters; resume via
 *       seL4_TCB_Resume. Guest MEMORY is read/written by the kernel double-mapping the guest's own
 *       Frame caps (which the kernel minted) into its own VSpace -- no ptrace peek/poke.
 *   (B) Loader ops   -- mmap == retype Untyped -> Frames + map into the guest VSpace; fork == copy
 *       the VSpace (or CoW-share frames); exec == parse the ELF and build a fresh VSpace; exit ==
 *       suspend the TCB + revoke its objects. The kernel IS the loader/pager.
 *   (C) Host driver  -- there is no fs/net/clock/tty in the seL4 kernel. Each pal_host_* is an IPC
 *       (seL4_Call) to a USERSPACE SERVER the AIOS kernel holds an endpoint cap to. Confinement
 *       stops being an openat2 trick and becomes STRUCTURAL: the fs server hands out a cap to the
 *       AIOS-root subtree, so an out-of-root path is simply UNNAMEABLE (the M4.2/M4.3/net-allow
 *       policies collapse into "which caps the guest's server-facing kernel was given").
 *
 * These three families are the recurring themes in the obligations below.
 *
 * ============================================================================================
 * SCAFFOLD RUNTIME BEHAVIOUR (honest)
 * ============================================================================================
 * The scaffold cannot run guests (there is no seL4). pal_guest_spawn prints a clear notice and
 * returns PAL_PID_NONE, so `aios-uk` (built PAL=sel4) starts, announces itself, and exits cleanly
 * without pretending to work. Every other primitive is defined (so the link is complete) and
 * returns its type's safe error sentinel; none is reached at runtime because spawn refuses first.
 * The scaffold's OWN diagnostic output uses the host's write(2) -- that is the SCAFFOLD writing a
 * message, not an seL4 obligation (a real port uses seL4_DebugPutChar during bring-up, then a
 * serial-server IPC); it is the one concession that keeps the proof binary observable.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>       /* write(2) -- ONLY for the scaffold's own diagnostic banner (see above) */

#include "pal.h"          /* the contract this file implements (pulls in aios_abi.h) */
#include "aios_version.h" /* the banner version, shared with the kernel */

/* --------------------------------------------------------------------------------------------
 * Scaffold diagnostics. NOT part of any seL4 obligation -- just a way for the proof binary to say
 * what it is. A real pal_sel4.c emits early output via seL4_DebugPutChar (debug kernel) or a
 * serial/console-server IPC; here we borrow the host's write(2) so the banner is visible.
 * -------------------------------------------------------------------------------------------- */
static void sel4_dbg(const char *s) {
    ssize_t n = write(2, s, strlen(s));
    (void)n;   /* best-effort diagnostics; nothing to do if the console write is short */
}

/* Marker returned/used by the never-reached stubs so their intent is unmistakable in the source:
 * "this is a scaffold stub; the seL4 obligation is in the comment above me; I am not implemented."
 * (Kept as a no-op the optimiser can drop; it never runs because pal_guest_spawn refuses first.) */
#define SEL4_STUB /* unimplemented in the scaffold -- see the proof obligation above */

/* ============================================================================================
 * guest world lifecycle (multi-process)
 * ============================================================================================ */

/*
 * pal_guest_spawn -- load + start the AIOS init program.
 *
 * seL4 OBLIGATION (the root-task bring-up, family A+B): construct the guest from capabilities --
 *   1. a fresh VSpace root (x86-64: a PML4; aarch64: a top-level page table) retyped from Untyped;
 *   2. parse the ELF at `path` (served by the fs server, family C) and, for each PT_LOAD segment,
 *      retype Untyped -> Frames, map them into the new VSpace (with intermediate page-table objects),
 *      and copy the segment bytes in via the kernel's own scratch mapping of those frames;
 *   3. build the initial stack (argc/argv/envp/auxv) the same way;
 *   4. retype a TCB, seL4_TCB_Configure it with the new VSpace, a CSpace (the guest's cap namespace,
 *      seeded with only what AIOS grants -- see confinement), an IPC buffer frame, and -- crucially --
 *      a FAULT ENDPOINT pointing at the AIOS kernel, so the guest's AIOS-ABI `svc` faults to us;
 *   5. set the entry PC + SP via seL4_TCB_WriteRegisters, then seL4_TCB_Resume.
 * The returned handle is opaque to the kernel (pal.h says so): a real port uses the TCB cap / a small
 * index into the kernel's guest table. VERIFICATION invariant: the guest's CSpace and VSpace name
 * ONLY objects AIOS minted for it -- the root of every later confinement guarantee.
 *
 * SCAFFOLD: there is no seL4 -- announce the scaffold and refuse to spawn (return PAL_PID_NONE, which
 * the kernel treats as "could not start init" and exits cleanly).
 */
pal_pid_t pal_guest_spawn(const char *path, char *const argv[]) {
    (void)path; (void)argv;
    sel4_dbg("[pal_sel4] AIOS v" AIOS_VERSION_STR " seL4 PAL -- the SEAM-PROVING SCAFFOLD (M6).\n");
    sel4_dbg("[pal_sel4] This is NOT a working seL4 backend. It exists to PROVE the AIOS kernel is\n");
    sel4_dbg("[pal_sel4] host-agnostic: kernel/aios_kernel.c compiles and LINKS against this third,\n");
    sel4_dbg("[pal_sel4] NON-Linux PAL (`make PAL=sel4`) with no Linux dependency. Every pal.h\n");
    sel4_dbg("[pal_sel4] primitive here is a documented stub stating its seL4 proof obligation.\n");
    sel4_dbg("[pal_sel4] Running guests needs a real seL4 port (SDK + target + the trap model in\n");
    sel4_dbg("[pal_sel4] docs/DESIGN_20260703_pal_sel4_seam.md). Refusing to spawn a guest.\n");
    return PAL_PID_NONE;
}

/*
 * pal_guest_next -- wait for the next event from ANY guest (the kernel's event loop primitive).
 *
 * seL4 OBLIGATION (family A, the crux of the trap model): the AIOS kernel seL4_Recv()s on the
 * endpoint(s) it configured as every guest's fault handler + its notification set. One receive
 * demultiplexes ALL of pal_guest_next's return codes: an unknown-syscall FAULT from a guest ->
 * code 1 (an AIOS syscall trapped; the fault message carries the guest's registers, so `sc` is
 * filled without a separate peek); a child-exit notification -> code 0; a socket-ready / timer /
 * console notification (from the net / timer / console servers, family C) -> code 4 / 3. This is
 * the seL4 analogue of waitpid()+ppoll(): ONE blocking wait over a unified event source, which is
 * exactly why pal.h shaped the loop as "wait for any, service one". No polling, no ptrace.
 *
 * SCAFFOLD: never reached (no guest ran); report "no live guests".
 */
int pal_guest_next(pal_pid_t *who, pal_syscall_t *sc, int *exit_code) {
    (void)who; (void)sc; (void)exit_code;
    return -1;   SEL4_STUB
}

/*
 * pal_take_term_signal -- return+clear a caught terminal signal (^C/^Z) for the controlling tty.
 *
 * seL4 OBLIGATION (family C): the CONSOLE/serial server watches the input stream and, on the
 * interrupt/suspend character, sends the AIOS kernel a notification carrying the signal; this
 * returns the last such signal. No host pty, no host signal disposition -- it is a message from
 * the terminal server the kernel holds an endpoint cap to.
 *
 * SCAFFOLD: no terminal, no signal.
 */
int pal_take_term_signal(void) {
    return 0;   SEL4_STUB
}

/*
 * pal_guest_return -- set the guest's syscall result and resume it toward its next syscall.
 *
 * seL4 OBLIGATION (family A): the guest is blocked in its fault (its AIOS `svc` faulted to us). To
 * "return a value and resume", the kernel seL4_Reply()s to the fault with a message that sets the
 * result register (x0 / rax) -- replying to a fault RESUMES the faulting thread with the supplied
 * registers, atomically completing the fault IPC. (seL4_TCB_WriteRegisters + seL4_TCB_Resume can also
 * resume the thread without the reply cap, but that is NOT equivalent: it resumes asynchronously
 * rather than atomically completing the fault, a scheduling difference a real port must weigh.)
 * This is the whole gVisor-style "service the trap, plant the result, run on" in one IPC.
 *
 * SCAFFOLD: never reached.
 */
int pal_guest_return(pal_pid_t who, uint64_t retval) {
    (void)who; (void)retval;
    return -1;   SEL4_STUB
}

/*
 * pal_guest_resume -- resume a guest WITHOUT setting a result (used after the inject primitives,
 * and to start a freshly spawned guest).
 *
 * seL4 OBLIGATION (family A): seL4_TCB_Resume(tcb) -- or, if the guest is blocked on its fault EP,
 * seL4_Reply with the registers the kernel already arranged (mmap address, fork result, handler
 * entry). Resume is decoupled from "return a value" for exactly the cases pal.h lists.
 *
 * SCAFFOLD: never reached.
 */
int pal_guest_resume(pal_pid_t who) {
    (void)who;
    return -1;   SEL4_STUB
}

/* ============================================================================================
 * guest memory
 * ============================================================================================ */

/*
 * pal_guest_read / pal_guest_write -- copy bytes out of / into a guest's address space.
 *
 * seL4 OBLIGATION (family A -- the answer to "no process_vm_readv"): the AIOS kernel MINTED every
 * frame in the guest's VSpace (family B), so it retains the Frame caps and keeps (or transiently
 * establishes) a mapping of those frames in its OWN VSpace. Reading/writing guest memory at `gaddr`
 * is then a plain memcpy through the kernel's scratch mapping of the frame backing that guest page
 * (walk the guest's page tables the kernel built, or a kernel-side gaddr->frame index). Note pal.h
 * requires writing into a DIFFERENT guest than the one trapped (a wait status into a parked parent);
 * that is fine -- the kernel holds every guest's frame caps. VERIFICATION invariant: the kernel maps
 * ONLY frames it owns for that guest -> no cross-guest or kernel/guest aliasing.
 *
 * SCAFFOLD: never reached; report 0 bytes copied.
 */
size_t pal_guest_read(pal_pid_t who, uint64_t gaddr, void *dst, size_t len) {
    (void)who; (void)gaddr; (void)dst; (void)len;
    return 0;   SEL4_STUB
}
size_t pal_guest_write(pal_pid_t who, uint64_t gaddr, const void *src, size_t len) {
    (void)who; (void)gaddr; (void)src; (void)len;
    return 0;   SEL4_STUB
}

/*
 * pal_guest_mmap -- grow a guest's address space by `len` bytes of anonymous memory.
 *
 * seL4 OBLIGATION (family B -- the answer to "no mmap injection"): the Linux PAL injects a real
 * mmap into the stopped tracee; seL4 has neither mmap nor a tracee to inject into. Instead the
 * kernel/pager: (1) seL4_Untyped_Retype()s Untyped memory into `len`/PAGE_SIZE Frame objects,
 * (2) chooses a free vaddr in the guest's VSpace and seL4_ARCH_Page_Map()s the frames there
 * (retyping intermediate page-table objects as needed), (3) returns that vaddr. The Untyped pool
 * IS the guest's memory budget -- a natural, capability-based memory quota. VERIFICATION invariant:
 * the new mapping neither aliases another guest's frames nor overlaps the guest's existing regions.
 *
 * SCAFFOLD: never reached; report allocation failure (0).
 */
uint64_t pal_guest_mmap(pal_pid_t who, size_t len) {
    (void)who; (void)len;
    return 0;   SEL4_STUB
}

/*
 * pal_guest_exec -- replace a guest's image with the AIOS program at `abspath` (a kernel-resolved
 * absolute path; argv/envp stay guest pointers).
 *
 * seL4 OBLIGATION (family B -- the kernel IS the loader): no execve to inject. The kernel opens
 * `abspath` via the fs server (family C; confinement makes an out-of-root path unnameable -- the
 * M4.3 guarantee, now structural), parses the ELF, tears down the guest's current VSpace frames
 * (revoke), builds a FRESH VSpace + stack exactly as in spawn, copies argv/envp across, and points
 * the guest's existing TCB at the new VSpace + entry PC. The kernel's fd table for the guest is
 * untouched (pal.h: AIOS fds survive exec), which on seL4 means the guest's server-facing caps
 * (family C) are preserved across the image swap.
 *
 * SCAFFOLD: never reached; report failure.
 */
int pal_guest_exec(pal_pid_t who, const char *abspath, uint64_t gargv, uint64_t genvp) {
    (void)who; (void)abspath; (void)gargv; (void)genvp;
    return -1;   SEL4_STUB
}

/*
 * pal_guest_fork -- create a child guest that copies the parent's address space.
 *
 * seL4 OBLIGATION (family B -- the answer to "no clone injection"): allocate a new VSpace + TCB,
 * then reproduce the parent's memory -- either eagerly (retype fresh Frames, copy each parent frame
 * through the kernel's scratch mappings) or, better, COPY-ON-WRITE: map the parent's frames
 * read-only into BOTH VSpaces and fault-copy on the first write (the kernel is the pager, so a
 * write fault arrives on the same fault EP -- a natural CoW). Duplicate the parent's server-facing
 * caps into the child's CSpace, share the fault EP, set the child's fork-result register to 0 and
 * the parent's to the child handle, resume both. This is where seL4's frame-cap model shines vs.
 * the Linux PAL's injected clone.
 *
 * SCAFFOLD: never reached; report failure.
 */
pal_pid_t pal_guest_fork(pal_pid_t parent) {
    (void)parent;
    return PAL_PID_NONE;   SEL4_STUB
}

/*
 * pal_guest_exit -- terminate a guest with an exit status.
 *
 * seL4 OBLIGATION (family B): seL4_TCB_Suspend the guest thread, then reclaim its objects --
 * seL4_CNode_Revoke/Delete the TCB, VSpace, page tables, and Frames back to Untyped (the memory
 * returns to the pool it came from). The subsequent "exit" the kernel observes (pal.h: it surfaces
 * through pal_guest_next as event 0) is the kernel's own bookkeeping notification, since the kernel
 * initiated the teardown. No host exit_group.
 *
 * SCAFFOLD: never reached.
 */
int pal_guest_exit(pal_pid_t who, int code) {
    (void)who; (void)code;
    return -1;   SEL4_STUB
}

/* ============================================================================================
 * host-driver gateway  (family C: every one of these is an IPC to a USERSPACE SERVER)
 *
 * seL4 has no filesystem, no network stack, no clock, no terminal in the kernel. The AIOS kernel
 * holds ENDPOINT CAPS to userspace servers (an fs server, a net server, a timer source, a console
 * server) and each pal_host_* below becomes a seL4_Call to the right one, marshalling arguments
 * into the IPC message + shared buffers. The backing objects the kernel keeps in its fd table
 * (pal_file_t) become per-connection caps / server-side handles. CONFINEMENT (M4.2/M4.3/net-allow)
 * becomes structural: a confined guest's kernel simply holds a cap to the AIOS-root SUBTREE (not
 * the whole fs) and an allow-list-parameterised net cap -- an out-of-root path or a disallowed peer
 * is UNNAMEABLE, so it need not be checked. The AIOS error codes pal.h mandates are host-agnostic;
 * a server maps its own failures onto the same AIOS_E* set.
 * ============================================================================================ */

/*
 * pal_host_std -- the host's standard streams (0=in 1=out 2=err) as backing handles.
 *
 * seL4 OBLIGATION: caps to the console server's stdin/stdout/stderr channels (during bring-up, the
 * debug console). The kernel seeds its reserved AIOS_FD_* from these.
 *
 * SCAFFOLD: return the fd number as the backing so the scaffold's own kputs diagnostics (which the
 * kernel routes to AIOS_FD_STDERR) reach a real console via pal_host_write below. This is the one
 * primitive the scaffold makes functional, and only for diagnostics.
 */
pal_file_t pal_host_std(int which) {
    return (pal_file_t)which;
}

/*
 * pal_host_open -- open a host-backed file; `aios_flags` are AIOS_O_*, the PAL translates them.
 *
 * seL4 OBLIGATION: seL4_Call the fs server with the (confined) path + translated flags; it returns
 * a per-file handle cap the kernel stores as the pal_file_t backing. Confinement = the server cap
 * roots at the AIOS subtree (family C).
 *
 * SCAFFOLD: never reached; report ENOSYS.
 */
pal_file_t pal_host_open(const char *path, uint64_t aios_flags, uint64_t mode) {
    (void)path; (void)aios_flags; (void)mode;
    return (pal_file_t)-AIOS_ENOSYS;   SEL4_STUB
}

/*
 * pal_host_read / pal_host_write / pal_host_close -- I/O on a backing object.
 *
 * seL4 OBLIGATION: seL4_Call the owning server (fs / net / console) with the handle + a shared
 * data buffer; bytes transferred come back in the reply. write is also the kernel's diagnostic +
 * stdout path -> the console server.
 *
 * SCAFFOLD: pal_host_write is made functional FOR THE STD STREAMS ONLY (0/1/2) via the host's
 * write(2), so the kernel banner + this backend's own notice are visible; every other backing, and
 * read/close, report failure (never reached -- no guest ran).
 */
long pal_host_read(pal_file_t f, void *buf, size_t len) {
    (void)f; (void)buf; (void)len;
    return -AIOS_ENOSYS;   SEL4_STUB
}
long pal_host_write(pal_file_t f, const void *buf, size_t len) {
    if (f == 0 || f == 1 || f == 2) {           /* the std streams -- diagnostics only (see header) */
        ssize_t n = write((int)f, buf, len);
        return (long)n;
    }
    (void)buf; (void)len;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_close(pal_file_t f) {
    (void)f;
    return 0;   SEL4_STUB   /* closing a scaffold backing is a no-op; a real port revokes the handle cap */
}

/*
 * pal_host_pipe -- a non-blocking unidirectional byte stream (rd/wr backings).
 *
 * seL4 OBLIGATION: build the pipe in the AIOS kernel itself -- a bounded ring buffer in kernel
 * memory with a Notification pair for readable/writable edges -- OR a pipe server. "Non-blocking"
 * becomes "the kernel parks the guest and services others" exactly as on Linux; the readiness edge
 * arrives as a notification (family C / the pal_guest_next co-wait).
 *
 * SCAFFOLD: never reached; report failure.
 */
int pal_host_pipe(pal_file_t *rd, pal_file_t *wr) {
    (void)rd; (void)wr;
    return -1;   SEL4_STUB
}

/*
 * pal_host_lseek / pal_host_fstat -- reposition / stat by backing.
 *
 * seL4 OBLIGATION: an fs-server IPC on the file handle; the server fills the host-agnostic
 * struct aios_stat directly (no host struct-stat translation -- the server speaks AIOS's format,
 * or the kernel translates once).
 *
 * SCAFFOLD: never reached.
 */
long long pal_host_lseek(pal_file_t f, long long off, int whence) {
    (void)f; (void)off; (void)whence;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_fstat(pal_file_t f, struct aios_stat *out) {
    (void)f; (void)out;
    return -AIOS_ENOSYS;   SEL4_STUB
}

/*
 * pal_host_getdents -- read directory entries (packed struct aios_dirent records).
 *
 * seL4 OBLIGATION: an fs-server "list directory" IPC returning entries the kernel packs into the
 * AIOS record format; confinement bounds it to the rooted subtree.
 *
 * SCAFFOLD: never reached.
 */
long pal_host_getdents(pal_file_t f, void *buf, size_t len) {
    (void)f; (void)buf; (void)len;
    return -AIOS_ENOSYS;   SEL4_STUB
}

/* ============================================================================================
 * filesystem namespace ops (by path)  -- all fs-server IPCs (family C), confined to the AIOS
 * subtree cap; a path that would escape the root is unnameable rather than clamped.
 * ============================================================================================ */

int pal_host_stat(const char *path, struct aios_stat *out, int follow) {
    (void)path; (void)out; (void)follow;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_unlink(const char *path) {
    (void)path;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_mkdir(const char *path, unsigned int mode) {
    (void)path; (void)mode;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_rmdir(const char *path) {
    (void)path;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_rename(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    return -AIOS_ENOSYS;   SEL4_STUB
}
/*
 * pal_host_chdir / pal_host_getcwd -- the per-process cwd is kernel-owned (the kernel pre-absolutes
 * paths), so on seL4 chdir is a verify-the-target IPC and getcwd only SEEDS init's cwd. pal.h notes
 * the tracer's real cwd never moves; on seL4 there is no tracer cwd at all, so getcwd seeds "/"
 * (confined) -- which the scaffold returns so the kernel's init-cwd seeding path is well-defined
 * even here (though spawn refuses before it runs).
 */
int pal_host_chdir(const char *path) {
    (void)path;
    return -AIOS_ENOSYS;   SEL4_STUB
}
long pal_host_getcwd(char *buf, size_t size) {
    if (buf && size >= 2) { buf[0] = '/'; buf[1] = '\0'; return 1; }   /* seed "/" (confined root) */
    return -AIOS_ENOSYS;   SEL4_STUB
}

/* ============================================================================================
 * the *at family (resolve a path relative to a directory backing)  -- fs-server IPCs taking a
 * directory handle (or PAL_AT_FDCWD); the server resolves within the confined subtree.
 * ============================================================================================ */

pal_file_t pal_host_openat(pal_file_t dir, const char *path, uint64_t aios_flags, uint64_t mode) {
    (void)dir; (void)path; (void)aios_flags; (void)mode;
    return (pal_file_t)-AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_fstatat(pal_file_t dir, const char *path, struct aios_stat *out, int follow) {
    (void)dir; (void)path; (void)out; (void)follow;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_unlinkat(pal_file_t dir, const char *path, int removedir) {
    (void)dir; (void)path; (void)removedir;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_faccessat(pal_file_t dir, const char *path, int amode) {
    (void)dir; (void)path; (void)amode;
    return -AIOS_ENOSYS;   SEL4_STUB
}
long pal_host_readlink(const char *path, char *buf, size_t bufsize) {
    (void)path; (void)buf; (void)bufsize;
    return -AIOS_ENOSYS;   SEL4_STUB
}

/* ============================================================================================
 * networking (host-passthrough on Linux; on seL4 an IPC to the NET SERVER)  -- a socket backing
 * is a per-connection cap to the net server. AIOS domain/type/protocol + sockaddr layouts are
 * host-agnostic constants the server understands. Non-blocking readiness (park/wake) arrives as
 * net-server notifications feeding the pal_guest_next co-wait (the pal_net_watch_* seam below).
 * CONFINEMENT (AIOS_NET_ALLOW / AIOS_NET_BIND_ALLOW) = an allow-list-parameterised net cap: a
 * disallowed peer/bind is refused by the server the guest's kernel was given.
 * ============================================================================================ */

pal_file_t pal_host_socket(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol;
    return (pal_file_t)-AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_connect(pal_file_t f, const void *addr, unsigned int addrlen) {
    (void)f; (void)addr; (void)addrlen;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_bind(pal_file_t f, const void *addr, unsigned int addrlen) {
    (void)f; (void)addr; (void)addrlen;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_listen(pal_file_t f, int backlog) {
    (void)f; (void)backlog;
    return -AIOS_ENOSYS;   SEL4_STUB
}
pal_file_t pal_host_accept(pal_file_t f, void *addr, unsigned int *addrlen) {
    (void)f; (void)addr; (void)addrlen;
    return (pal_file_t)-AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_setsockopt(pal_file_t f, int level, int optname, const void *optval, unsigned int optlen) {
    (void)f; (void)level; (void)optname; (void)optval; (void)optlen;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_getsockname(pal_file_t f, void *addr, unsigned int *addrlen) {
    (void)f; (void)addr; (void)addrlen;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_sock_error(pal_file_t f) {
    (void)f;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_sock_writable(pal_file_t f) {
    (void)f;
    return -AIOS_ENOSYS;   SEL4_STUB
}

/* ============================================================================================
 * socket-readiness seam (park/wake)  -- the kernel publishes the sockets its parked guests await;
 * the trap front-end co-waits guest events AND that readiness. On seL4 the "co-wait" is the same
 * seL4_Recv as pal_guest_next: net-server readiness + a timer expiry arrive as notifications on the
 * endpoint set the kernel already waits on, so this seam maps to badged notification caps. `ms` is
 * a deadline the timer source honours (MCS timeout / a timer-server notification).
 * ============================================================================================ */

void pal_net_watch_reset(void) {
    SEL4_STUB   /* the scaffold has no watch set to clear */
}
void pal_net_watch_add(pal_file_t f, int want_write) {
    (void)f; (void)want_write;
    SEL4_STUB
}
void pal_net_watch_timeout(int ms) {
    (void)ms;
    SEL4_STUB
}
int pal_net_have_watches(void) {
    return 0;   SEL4_STUB   /* never any watches -- no guest ran */
}
int pal_net_wait_ready(void) {
    return 0;   SEL4_STUB   /* never called: pal_guest_next uses this only while there are watches */
}

/* ============================================================================================
 * terminal + clock  (family C)
 * ============================================================================================ */

/*
 * pal_host_isatty / tcgetattr / tcsetattr -- terminal line discipline.
 *
 * seL4 OBLIGATION: a CONSOLE/terminal server owns the line discipline; these are IPCs to it. When a
 * guest sets raw mode the server stops cooking input, so the kernel's reads return char-at-a-time
 * (same emergent behaviour pal.h describes on Linux). struct aios_termios is host-agnostic; the
 * server speaks it (or the kernel translates once, as the Linux PAL does).
 */
int pal_host_isatty(pal_file_t f) {
    (void)f;
    return 0;   SEL4_STUB
}
int pal_host_tcgetattr(pal_file_t f, struct aios_termios *out) {
    (void)f; (void)out;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_tcsetattr(pal_file_t f, int actions, const struct aios_termios *in) {
    (void)f; (void)actions; (void)in;
    return -AIOS_ENOSYS;   SEL4_STUB
}

/*
 * pal_host_clock_gettime -- read REALTIME / MONOTONIC into a host-agnostic struct aios_timespec.
 *
 * seL4 OBLIGATION: the kernel's only time source. MONOTONIC comes from the platform timer (a timer
 * driver reading the generic timer / TSC, or seL4 MCS's notion of time); REALTIME is that plus a
 * boot-time offset a time server supplies (seL4 has no RTC in-kernel, just as AIOS on the RPi had
 * none -- SNTP at boot filled it). An IPC to the timer/time server, or a shared read-only time page.
 *
 * SCAFFOLD: never reached.
 */
int pal_host_clock_gettime(int clk_id, struct aios_timespec *out) {
    (void)clk_id; (void)out;
    return -AIOS_ENOSYS;   SEL4_STUB
}

/* ============================================================================================
 * file-metadata *at family (mode / owner / symlink / hardlink / times)  -- fs-server IPCs; the
 * confinement subtlety pal.h documents (resolve single-target ops inside the root FIRST so a
 * planted symlink can't redirect a metadata change) is automatic on seL4: the guest's fs cap can
 * only name in-subtree objects, so there is no escaping symlink to resolve away.
 * ============================================================================================ */

int pal_host_fchmodat(pal_file_t dir, const char *path, unsigned int mode, int nofollow) {
    (void)dir; (void)path; (void)mode; (void)nofollow;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_fchownat(pal_file_t dir, const char *path, unsigned int owner, unsigned int group, int nofollow) {
    (void)dir; (void)path; (void)owner; (void)group; (void)nofollow;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_symlinkat(const char *target, pal_file_t newdir, const char *linkpath) {
    (void)target; (void)newdir; (void)linkpath;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_linkat(pal_file_t olddir, const char *oldpath, pal_file_t newdir, const char *newpath, int follow) {
    (void)olddir; (void)oldpath; (void)newdir; (void)newpath; (void)follow;
    return -AIOS_ENOSYS;   SEL4_STUB
}
int pal_host_utimensat(pal_file_t dir, const char *path, const struct aios_timespec *times, int nofollow) {
    (void)dir; (void)path; (void)times; (void)nofollow;
    return -AIOS_ENOSYS;   SEL4_STUB
}

/* ============================================================================================
 * signal delivery (register manipulation lives in the PAL because it is host-specific)
 * ============================================================================================ */

/*
 * pal_guest_setret -- set the guest's syscall result WITHOUT resuming (it stays at the syscall exit).
 *
 * seL4 OBLIGATION (family A): seL4_TCB_WriteRegisters to set the result register while leaving the
 * thread suspended (do NOT reply/resume). The kernel uses this to interpose signal delivery between
 * finishing a syscall and resuming -- see pal_guest_deliver.
 *
 * SCAFFOLD: never reached.
 */
int pal_guest_setret(pal_pid_t who, uint64_t retval) {
    (void)who; (void)retval;
    return -1;   SEL4_STUB
}

/*
 * pal_guest_deliver / pal_guest_sigreturn -- run a guest signal handler, then restore.
 *
 * seL4 OBLIGATION (family A -- the frame dance without ptrace): to DELIVER, the kernel
 * seL4_TCB_ReadRegisters(guest) into `savebuf` (opaque, >= PAL_SIGSAVE_SIZE), then
 * seL4_TCB_WriteRegisters to set pc=handler, arg0=signum, return-address=tramp, and resumes. When
 * the handler returns it traps via the trampoline (AIOS_SYS_SIGRETURN, another fault); SIGRETURN
 * then seL4_TCB_WriteRegisters the saved `savebuf` back so the guest resumes after the interrupted
 * syscall. Building the handler's stack frame (if any) uses the same cap-mapped memory writes as
 * pal_guest_write. `savebuf` stays host-agnostic bytes the kernel keeps per-process -- on seL4 it
 * holds a seL4_UserContext rather than a Linux user_regs_struct, but the kernel never inspects it.
 *
 * SCAFFOLD: never reached.
 */
int pal_guest_deliver(pal_pid_t who, uint64_t handler, uint64_t signum, uint64_t tramp, void *savebuf) {
    (void)who; (void)handler; (void)signum; (void)tramp; (void)savebuf;
    return -1;   SEL4_STUB
}
int pal_guest_sigreturn(pal_pid_t who, const void *savebuf) {
    (void)who; (void)savebuf;
    return -1;   SEL4_STUB
}

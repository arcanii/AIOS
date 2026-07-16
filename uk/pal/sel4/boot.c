/*
 * uk/pal/sel4/boot.c -- the REAL seL4 PAL backend. It boots the host-agnostic AIOS kernel
 * (uk/kernel/aios_kernel.c, compiled -Dmain=aios_kernel_main) as a seL4 ROOT TASK and services its
 * guests. It grows phase by phase (docs/PLAN_20260709_sel4_real_port.md).
 *
 * Phase A: console (pal_host_write -> seL4_DebugPutChar) + a root-task main(); pal_guest_spawn refused.
 * Phase B: FAMILY A -- the fault-EP trap loop. A guest is a seL4 thread; its AIOS `svc`
 *   UnknownSyscall-FAULTS to the root task (seL4/aarch64 reads the syscall nr from x7, which Phase 0
 *   pinned to the AIOS number -- x7 >= 0x1000 is never a valid (negative) seL4 syscall, so every AIOS
 *   svc deterministically faults). One seL4_Recv is the whole event source; a reply resumes the guest.
 * Phase C.1: FAMILY B begins -- pal_guest_mmap (vspace_new_pages: Untyped->Frames mapped into the
 *   guest VSpace) + the case-(b) RESUME-PAST-SYSCALL reply (an inject primitive stashes its result;
 *   pal_guest_resume replies FaultIP+4 with it).
 * Phase C.2 (THIS): the PROCESS TABLE + pal_guest_fork. The single-guest globals become a table of
 *   guests, each with a BADGED copy (badge = slot+1) of the ONE master fault EP -- the badge of the
 *   fault-handler cap installed in the faulter's cspace is delivered with every fault
 *   (kernel/faulthandler.c sendFaultIPC passes capEPBadge; sel4utils COPIES our pre-badged cap into
 *   the child, preserving it -- libsel4utils/src/process.c:509/439/129). Because non-MCS Recv DELETES
 *   an unconsumed caller cap (syscall.c handleRecv -> deleteCallerCap), every fault's implicit reply
 *   cap is immediately seL4_CNode_SaveCaller'd into a per-guest slot; replies go out via seL4_Send on
 *   the saved slot, which funnels into the SAME doReplyTransfer as an in-place seL4_Reply (kernel
 *   objecttype.c:701-714/819-823) and consumes the cap. That is what lets a parent PARK in wait()
 *   across other guests' faults and still be woken later -- the load-bearing C.2 mechanism.
 *   FORK is an EAGER copy (the plan's tripwire default; COW is a later optimization): configure a
 *   fresh process from the SAME CPIO ELF (text/rodata/data/bss reloaded + placed identically -- the
 *   second configure is deterministic first-fit over identical reservations; we VERIFY the stack
 *   landed where the parent's is before copying by address), then overwrite what diverged: the
 *   writable PT_LOAD ranges (parsed from the ELF's own phdrs -- sel4utils' preload path throws its
 *   region records away, libsel4utils/src/elf.c:537-541), the whole 16-page stack
 *   (CONFIG_SEL4UTILS_STACK_SIZE, eagerly mapped, guard page BELOW stack_lo), and the parent's mmap
 *   regions (replicated at IDENTICAL vaddrs via vspace_reserve_range_at + vspace_new_pages_at_vaddr).
 *   The child's registers are the parent's FULL 36-word seL4_UserContext (seL4_TCB_ReadRegisters is
 *   legal on a fault-blocked thread and reads the saved context -- the 13-MR fault message alone
 *   lacks x8..x30) with x0=0 and pc=FaultIP+4 (the +4 is load-bearing: WriteRegisters' pc lands in
 *   the FaultIP register and Restart runs there -- an unadvanced pc re-runs the svc and re-faults).
 *   WriteRegisters(resume=0) leaves the child Inactive; the kernel registers it and pal_guest_resume
 *   gives it the same spawn-kick (seL4_TCB_Resume) a fresh guest gets. The parent's fork return value
 *   (the child pid) rides the proven inject stash. Object TEARDOWN also lands here: a reported exit
 *   destroys the process (sel4utils_destroy_process frees the TCB/CSpace/VSpace INCLUDING our
 *   post-configure mmap frames -- vspace_tear_down frees any page with a nonzero ut cookie -- and
 *   leaves our caller-owned fault-EP object alone, own_ep=false) + our badged EP copy + the reply slot.
 *
 * The design + every seL4 API used here is kernel-source-validated (the Phase B + C.2 research; all
 * claims re-verified adversarially against deps/kernel + deps/seL4_libs). Key facts carried forward:
 *   - Fault msg (aarch64 seL4_UnknownSyscall_Msg): X0..X7 = MR0..7, FaultIP=8, SP=9, LR=10, SPSR=11,
 *     Syscall=12, Length=13.  The AIOS nr is MR7 (X7).  Args are X0..X5.
 *   - To RESUME an UnknownSyscall fault: reply label 0, length 9, MRs = the faulted frame with
 *     X0<-retval and FaultIP<-FaultIP+4. A fault reply writes at most the first 12 MRs (registers);
 *     length 9 stops before SP/LR/SPSR (the LR reply slot actually maps to ELR_EL1 -- never echo it).
 *   - seL4_CNode_SaveCaller MOVES the caller cap; the dest slot must be EMPTY (else seL4_DeleteFirst),
 *     and the no-caller case SILENTLY "succeeds" (kernel cnode.c:364-390 returns EXCEPTION_NONE) --
 *     our invariant makes it moot: a fault was just Recv'd, so the caller cap is always present.
 *   - Guest memory (the seL4 answer to process_vm_readv): vspace_access_page_with_callback duplicates
 *     the ONE guest frame containing an address into the root vspace (the STORED frame caps are the
 *     full-rights originals, so this works for elf-loaded, stack, and new_pages pages alike) and hands
 *     the callback the mapped vaddr; cacheable=1 matches the guest ([[feedback_pipe_shm_cache]]).
 *
 * The uk/pal/pal_sel4.c SCAFFOLD stays the `make PAL=sel4` link-purity canary; THIS file is the one
 * that boots + runs guests.
 */
#include <sel4/sel4.h>
#include <sel4platsupport/bootinfo.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>
#include <simple/simple.h>
#include <simple-default/simple-default.h>
#include <vka/object.h>
#include <vspace/vspace.h>
#include <sel4utils/vspace.h>
#include <sel4utils/process.h>
#include <sel4utils/process_config.h>
#include <elf/elf.h>             /* fork parses the guest ELF's phdrs (writable PT_LOAD ranges) */
#include <cpio/cpio.h>           /* ... from the linked-in CPIO (the same archive sel4utils loads) */
#include <string.h>              /* memcpy -- muslc (the root task links the C runtime) */

#include "pal.h"                 /* the contract (pulls in aios_abi.h) -- boot.c uses no AIOS version
                                  * macros; the kernel (aios_kernel.c) prints the version banner. */

/* aios_kernel.c's main(), renamed by -Dmain=aios_kernel_main (build plumbing; source byte-identical). */
int aios_kernel_main(int argc, char **argv);

/* The guest archive MakeCPIO linked in (guests_cpio.o) -- the same globals sel4utils reads. */
extern char _cpio_archive[];
extern char _cpio_archive_end[];

/* ---- the bring-up console: seL4_DebugPutChar on the debug (printing) kernel (Phase A) ------------- */
static void con_write(const char *s, size_t n) { for (size_t i = 0; i < n; i++) seL4_DebugPutChar(s[i]); }
static void con_puts(const char *s) { size_t n = 0; while (s[n]) n++; con_write(s, n); }
static void con_put_int(long v) {
    char b[24]; int i = (int)sizeof b; b[--i] = '\0';
    unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) b[--i] = '0';
    while (u) { b[--i] = (char)('0' + (u % 10)); u /= 10; }
    if (v < 0) b[--i] = '-';
    con_puts(&b[i]);
}
static void con_put_hex(uint64_t u) {
    char b[19]; int i = (int)sizeof b; b[--i] = '\0';
    if (u == 0) b[--i] = '0';
    while (u) { int d = (int)(u & 0xf); b[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u >>= 4; }
    con_puts("0x"); con_puts(&b[i]);
}

/* ================================ root-task allocator state (Phase B) ============================== */
#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 8000)   /* the proven 0.4.x root pool (~32MB) */
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];
static sel4utils_alloc_data_t vspace_data;
static simple_t   simple;
static vka_t      vka;
static vspace_t   vspace;
static allocman_t *allocman;
static int        g_boot_done = 0;

static int boot_once(void) {
    if (g_boot_done) return 0;
    seL4_BootInfo *info = platsupport_get_bootinfo();
    if (!info) { con_puts("[pal_sel4] FATAL: no bootinfo\n"); return -1; }
    simple_default_init_bootinfo(&simple, info);
    allocman = bootstrap_use_current_simple(&simple, ALLOCATOR_STATIC_POOL_SIZE, allocator_mem_pool);
    if (!allocman) { con_puts("[pal_sel4] FATAL: allocman bootstrap\n"); return -1; }
    allocman_make_vka(&vka, allocman);
    int err = sel4utils_bootstrap_vspace_with_bootinfo_leaky(&vspace, &vspace_data,
                                                             simple_get_pd(&simple), &vka, info);
    if (err) { con_puts("[pal_sel4] FATAL: vspace bootstrap\n"); return -1; }
    g_boot_done = 1;
    return 0;
}

/* ================================ the process table (Phase C.2) ==================================== */
#define MAX_GUESTS   16          /* PAL capacity this phase (the kernel's own table holds 64) */
#define MAX_MMAPS    32          /* anonymous regions per guest (libaios malloc grows by whole mmaps) */
#define MAX_WSEGS     8          /* writable PT_LOAD ranges per ELF (static -nostdlib images have ~1) */
#define PAGE_SZ      ((size_t)BIT(seL4_PageBits))

typedef struct guest {
    int        used;
    pal_pid_t  pid;              /* MONOTONIC (g_next_pid) -- never reused, so the kernel's zombie
                                  * bookkeeping can never alias a dead pid with a fresh guest */
    sel4utils_process_t proc;
    char       image[64];        /* the CPIO image name -- fork reloads the parent's */
    cspacepath_t fault_ep_path;  /* our badged copy of g_fault_ep (badge = slot+1) */
    cspacepath_t reply_path;     /* the SaveCaller slot: this guest's in-flight reply cap */
    int        have_reply;       /* a saved, unconsumed reply cap sits in reply_path */
    seL4_Word  fregs[seL4_UnknownSyscall_Length];   /* the pending fault's register snapshot */
    int        started;          /* 0 = Inactive (spawn/fork kick pending); 1 = has run */
    uint64_t   inject_x0;        /* an inject primitive's result (mmap addr, fork's child pid) ... */
    int        have_inject;      /* ... consumed by pal_guest_resume's case-(b) reply */
    int        pending_exit;     /* exit accepted (TCB suspended); report + teardown on next next() */
    int        exit_code;
    /* the memory map fork must copy: writable ELF ranges + the stack + the mmap regions */
    uint64_t   wseg_lo[MAX_WSEGS], wseg_hi[MAX_WSEGS];   /* page-aligned [lo,hi) */
    int        nwseg;
    uint64_t   mm_vaddr[MAX_MMAPS]; size_t mm_pages[MAX_MMAPS];
    int        nmm;
    uint64_t   stack_lo, stack_hi;                       /* [lo,hi), eagerly mapped; guard is below */
} guest_t;

static guest_t     g_g[MAX_GUESTS];
static int         g_nused    = 0;
static pal_pid_t   g_next_pid = 1;
static vka_object_t g_fault_ep;          /* the ONE master endpoint; guests get badged copies */
static int          g_have_ep = 0;

static guest_t *gfind(pal_pid_t pid) {
    for (int i = 0; i < MAX_GUESTS; i++) if (g_g[i].used && g_g[i].pid == pid) return &g_g[i];
    return NULL;
}
static int gslot(const guest_t *g) { return (int)(g - g_g); }

/* Parse the image's writable PT_LOAD ranges (page-aligned) from the CPIO ELF. This is fork's copy
 * list: sel4utils' preload path loads every segment eagerly but throws its region records away
 * (elf.c load_record_regions frees them), and its preload=false path reserves at vspace-CHOSEN
 * addresses (elf_reserve hardcodes mapanywhere=1) -- so the ELF's own phdrs are the only truthful
 * source of "which pages can have diverged from the file image". */
static int load_wsegs(guest_t *g) {
    unsigned long size = 0;
    const void *file = cpio_get_file(_cpio_archive,
                                     (unsigned long)(_cpio_archive_end - _cpio_archive),
                                     g->image, &size);
    if (!file) { con_puts("[pal_sel4] fork: CPIO lookup failed\n"); return -1; }
    elf_t elf;
    if (elf_newFile(file, size, &elf)) { con_puts("[pal_sel4] fork: bad guest ELF\n"); return -1; }
    g->nwseg = 0;
    for (size_t i = 0; i < elf_getNumProgramHeaders(&elf); i++) {
        if (elf_getProgramHeaderType(&elf, i) != 1 /* PT_LOAD */) continue;
        if (!(elf_getProgramHeaderFlags(&elf, i) & 2 /* PF_W */)) continue;
        if (g->nwseg >= MAX_WSEGS) { con_puts("[pal_sel4] fork: too many writable segments\n"); return -1; }
        uint64_t va = (uint64_t)elf_getProgramHeaderVaddr(&elf, i);
        uint64_t end = va + (uint64_t)elf_getProgramHeaderMemorySize(&elf, i);   /* memsz: data+bss */
        g->wseg_lo[g->nwseg] = va & ~((uint64_t)PAGE_SZ - 1);
        g->wseg_hi[g->nwseg] = (end + PAGE_SZ - 1) & ~((uint64_t)PAGE_SZ - 1);
        g->nwseg++;
    }
    return 0;
}

/* Build a guest process in slot `g`: a badged fault-EP copy (badge = slot+1), a SaveCaller slot, and
 * a configured sel4utils process loaded (preload) from the CPIO image. Shared by spawn and fork.
 * On failure everything allocated here is unwound. */
static int guest_build(guest_t *g, const char *image) {
    size_t n = 0; while (image[n] && n < sizeof g->image - 1) { g->image[n] = image[n]; n++; }
    g->image[n] = '\0';

    /* the badged fault-EP copy: the badge is how pal_guest_next tells the faulters apart */
    cspacepath_t src;
    vka_cspace_make_path(&vka, g_fault_ep.cptr, &src);
    if (vka_cspace_alloc_path(&vka, &g->fault_ep_path)) return -1;
    if (seL4_CNode_Mint(g->fault_ep_path.root, g->fault_ep_path.capPtr, g->fault_ep_path.capDepth,
                        src.root, src.capPtr, src.capDepth,
                        seL4_AllRights, (seL4_Word)(gslot(g) + 1))) {
        vka_cspace_free(&vka, g->fault_ep_path.capPtr);
        return -1;
    }
    /* the reply slot (empty -- SaveCaller requires that) */
    if (vka_cspace_alloc_path(&vka, &g->reply_path)) {
        seL4_CNode_Delete(g->fault_ep_path.root, g->fault_ep_path.capPtr, g->fault_ep_path.capDepth);
        vka_cspace_free(&vka, g->fault_ep_path.capPtr);
        return -1;
    }

    vka_object_t badged_ep;
    memset(&badged_ep, 0, sizeof badged_ep);
    badged_ep.cptr = g->fault_ep_path.capPtr;            /* by-cptr handoff; we keep ownership
                                                          * (process_config_fault_endpoint sets
                                                          * create_fault_endpoint=false -> own_ep
                                                          * stays false -> destroy leaves it alone) */
    sel4utils_process_config_t config = process_config_new(&simple);
    config = process_config_elf(config, g->image, true /* preload: load the ELF eagerly now */);
    config = process_config_create_cnode(config, 12);
    config = process_config_create_vspace(config, NULL, 0);
    config = process_config_auth(config, simple_get_tcb(&simple));
    config = process_config_fault_endpoint(config, badged_ep);

    int err = sel4utils_configure_process_custom(&g->proc, &vka, &vspace, config);
    if (err) {
        con_puts("[pal_sel4] configure_process failed: "); con_put_int(err); con_puts("\n");
        seL4_CNode_Delete(g->fault_ep_path.root, g->fault_ep_path.capPtr, g->fault_ep_path.capDepth);
        vka_cspace_free(&vka, g->fault_ep_path.capPtr);
        vka_cspace_free(&vka, g->reply_path.capPtr);
        return -1;
    }

    if (load_wsegs(g)) {         /* the ELF just configured fine, so this only fails on table limits */
        sel4utils_destroy_process(&g->proc, &vka);
        seL4_CNode_Delete(g->fault_ep_path.root, g->fault_ep_path.capPtr, g->fault_ep_path.capDepth);
        vka_cspace_free(&vka, g->fault_ep_path.capPtr);
        vka_cspace_free(&vka, g->reply_path.capPtr);
        return -1;
    }
    /* stack_size is recorded in 4K pages (sel4utils thread.c); the guard page is BELOW stack_lo and
     * unmapped, so [stack_lo, stack_hi) is exactly the eagerly-mapped range */
    g->stack_hi = (uint64_t)(uintptr_t)g->proc.thread.stack_top;
    g->stack_lo = g->stack_hi - (uint64_t)g->proc.thread.stack_size * PAGE_SZ;

    g->have_reply = 0; g->started = 0; g->have_inject = 0; g->inject_x0 = 0;
    g->pending_exit = 0; g->exit_code = 0; g->nmm = 0;
    return 0;
}

/* Destroy a guest's objects: the process (TCB/CSpace/VSpace + every frame with a ut cookie -- ELF
 * pages, stack, IPC buffer, AND our vspace_new_pages mmap frames), then our badged EP copy and the
 * reply slot. The caller has already suspended the TCB and deleted/consumed any saved reply cap --
 * and owns the used/g_nused accounting (an unwound half-built guest was never counted). */
static void guest_destroy_objects(guest_t *g) {
    sel4utils_destroy_process(&g->proc, &vka);
    seL4_CNode_Delete(g->fault_ep_path.root, g->fault_ep_path.capPtr, g->fault_ep_path.capDepth);
    vka_cspace_free(&vka, g->fault_ep_path.capPtr);
    vka_cspace_free(&vka, g->reply_path.capPtr);
}

/* ================================ Phase A: console (kept) ========================================== */
pal_file_t pal_host_std(int which) { return (pal_file_t)which; }

long pal_host_write(pal_file_t f, const void *buf, size_t len) {
    if (f == 0 || f == 1 || f == 2) { con_write((const char *)buf, len); return (long)len; }
    return -AIOS_ENOSYS;
}

long pal_host_getcwd(char *buf, size_t size) {
    if (buf && size >= 2) { buf[0] = '/'; buf[1] = '\0'; return 1; }
    return -AIOS_ENOSYS;
}

/* ================================ the trap loop (families A+B) ===================================== */

/* spawn: load the C.2 test guest from the CPIO into a fresh table slot and leave it SUSPENDED so the
 * kernel registers it before it runs toward its first svc (pal_guest_resume then starts it). Still
 * ignores `path` -- the init guest is hardcoded until C.3/C.4 bring exec + the tarfs. */
pal_pid_t pal_guest_spawn(const char *path, char *const argv[]) {
    (void)argv;
    if (boot_once()) return PAL_PID_NONE;
    if (!g_have_ep) {
        if (vka_alloc_endpoint(&vka, &g_fault_ep)) { con_puts("[pal_sel4] fault-ep alloc failed\n"); return PAL_PID_NONE; }
        g_have_ep = 1;
    }
    guest_t *g = NULL;
    for (int i = 0; i < MAX_GUESTS; i++) if (!g_g[i].used) { g = &g_g[i]; break; }
    if (!g) { con_puts("[pal_sel4] spawn: guest table full\n"); return PAL_PID_NONE; }

    con_puts("[pal_sel4] Phase C.2: loading guest 'guest_fork' from the CPIO (kernel asked for ");
    con_puts(path); con_puts(")\n");

    if (guest_build(g, "guest_fork")) return PAL_PID_NONE;

    int err = sel4utils_spawn_process_v(&g->proc, &vka, &vspace, 0, NULL, 0 /* resume=0: suspended */);
    if (err) {
        con_puts("[pal_sel4] spawn_process failed: "); con_put_int(err); con_puts("\n");
        guest_destroy_objects(g);
        return PAL_PID_NONE;
    }

    g->used = 1;
    g->pid  = g_next_pid++;
    g_nused++;
    return g->pid;
}

/* reply_resume: resume a FAULT-STOPPED guest past its serviced svc via its SAVED reply cap -- a
 * seL4_Send on the saved slot funnels into the same doReplyTransfer as an in-place seL4_Reply
 * (label 0, length 9: X0 = the result, X1..X7 echoed from the fault snapshot, FaultIP+4 to step past
 * the svc) and CONSUMES the cap, leaving the slot empty for the guest's next fault. Rebuilds every MR
 * from the per-guest fregs snapshot because the service path used the IPC buffer. */
static void reply_resume(guest_t *g, uint64_t x0) {
    if (!g->have_reply) {         /* contract violation -- resuming a guest that is not fault-stopped */
        con_puts("[pal_sel4] BUG: reply_resume without a saved reply cap (pid ");
        con_put_int((long)g->pid); con_puts(")\n");
        return;
    }
    seL4_SetMR(seL4_UnknownSyscall_X0, x0);
    for (int i = seL4_UnknownSyscall_X1; i <= seL4_UnknownSyscall_X7; i++) seL4_SetMR(i, g->fregs[i]);
    seL4_SetMR(seL4_UnknownSyscall_FaultIP, g->fregs[seL4_UnknownSyscall_FaultIP] + 4);   /* past the svc */
    seL4_Send(g->reply_path.capPtr,
              seL4_MessageInfo_new(0, 0, 0, seL4_UnknownSyscall_FaultIP + 1 /* length 9 */));
    g->have_reply = 0;
}

/* resume: (a) the spawn/fork kick -- start an Inactive guest (a fresh spawn at its ELF entry, or a
 * fork child at its pre-written parent context; seL4_TCB_Resume runs an Inactive thread at whatever
 * pc its registers hold) -- or (b) resume a guest stopped at a serviced syscall, past an INJECT
 * primitive (mmap/fork stash their own x0) or a setret interpose (stashed into fregs[X0]). */
int pal_guest_resume(pal_pid_t who) {
    guest_t *g = gfind(who);
    if (!g) return -1;
    if (!g->started) {                             /* (a) first run: Inactive -> running */
        g->started = 1;
        seL4_TCB_Resume(g->proc.thread.tcb.cptr);
        return 0;
    }
    uint64_t x0 = g->have_inject ? g->inject_x0 : g->fregs[seL4_UnknownSyscall_X0];  /* (b) */
    g->have_inject = 0;
    reply_resume(g, x0);
    return 0;
}

/* next: report a pending exit (event 0, + teardown), else block on the master fault EP for ANY
 * guest's next syscall (event 1) or death (event 0). The badge (slot+1, from the guest's badged
 * fault-handler cap) identifies the faulter; the implicit reply cap is IMMEDIATELY SaveCaller'd into
 * the guest's slot so a later Recv cannot strand it (non-MCS deletes unconsumed caller caps). */
int pal_guest_next(pal_pid_t *who, pal_syscall_t *sc, int *exit_code) {
    for (int i = 0; i < MAX_GUESTS; i++) {
        guest_t *g = &g_g[i];
        if (g->used && g->pending_exit) {
            *who = g->pid; *exit_code = g->exit_code;
            guest_destroy_objects(g);
            g->used = 0; g_nused--;
            return 0;
        }
    }
    if (g_nused == 0) return -1;

    for (;;) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t tag = seL4_Recv(g_fault_ep.cptr, &badge);
        if (badge < 1 || badge > MAX_GUESTS || !g_g[badge - 1].used) {
            con_puts("[pal_sel4] stray fault-EP message, badge="); con_put_int((long)badge);
            con_puts(" -- dropped\n");
            continue;                              /* its unconsumed caller cap dies on the next Recv */
        }
        guest_t *g = &g_g[badge - 1];
        /* Move the one-slot implicit reply cap out of harm's way BEFORE anything else can Recv.
         * A fault always arrives as a Call (sendFaultIPC do_call=true), so the caller cap is present;
         * the empty-slot precondition holds because a guest faults again only after its previous
         * reply was consumed (Send) or discarded (exit's Delete). */
        seL4_CNode_SaveCaller(g->reply_path.root, g->reply_path.capPtr, g->reply_path.capDepth);
        g->have_reply = 1;

        if (seL4_isUnknownSyscall_tag(tag)) {
            for (int i = 0; i < seL4_UnknownSyscall_Length; i++) g->fregs[i] = seL4_GetMR(i);
            *who = g->pid;
            sc->nr     = g->fregs[seL4_UnknownSyscall_X7];   /* the AIOS nr (Phase 0 pinned x7) */
            sc->arg[0] = g->fregs[seL4_UnknownSyscall_X0];
            sc->arg[1] = g->fregs[seL4_UnknownSyscall_X1];
            sc->arg[2] = g->fregs[seL4_UnknownSyscall_X2];
            sc->arg[3] = g->fregs[seL4_UnknownSyscall_X3];
            sc->arg[4] = g->fregs[seL4_UnknownSyscall_X4];
            sc->arg[5] = g->fregs[seL4_UnknownSyscall_X5];
            return 1;
        }

        /* Any other fault (a real VM fault, a cap fault, ...) kills the guest: report a death so the
         * kernel's reap bookkeeping runs. Discard the saved reply cap (never resume a crashed guest). */
        con_puts("[pal_sel4] guest pid "); con_put_int((long)g->pid);
        con_puts(" non-syscall fault label="); con_put_int((long)seL4_MessageInfo_get_label(tag));
        con_puts(" -- terminating guest\n");
        seL4_TCB_Suspend(g->proc.thread.tcb.cptr);
        seL4_CNode_Delete(g->reply_path.root, g->reply_path.capPtr, g->reply_path.capDepth);
        g->have_reply = 0;
        *who = g->pid; *exit_code = 139;           /* 128 + SIGSEGV-ish */
        guest_destroy_objects(g);
        g->used = 0; g_nused--;
        return 0;
    }
}

/* return: set the guest's syscall result and resume it past the svc (a fault-stopped guest). Also
 * used to WAKE a PARKED guest (wait/read blocked) -- its saved reply cap is exactly what survives
 * the other guests' intervening faults. */
int pal_guest_return(pal_pid_t who, uint64_t retval) {
    guest_t *g = gfind(who);
    if (!g) return -1;
    reply_resume(g, retval);
    return 0;
}

/* setret: set the result but do NOT resume (the kernel interposes signal delivery: setret, then
 * deliver/sigreturn or resume). The stash lands in this guest's fregs[X0], which the case-(b)
 * no-inject resume replies with -- so setret + a later pal_guest_resume round-trips per-guest now.
 * Full signal delivery (deliver/sigreturn) stays Phase E. */
int pal_guest_setret(pal_pid_t who, uint64_t retval) {
    guest_t *g = gfind(who);
    if (!g) return -1;
    g->fregs[seL4_UnknownSyscall_X0] = retval;
    return 0;
}

/* exit: terminate the guest. Suspend its TCB, DISCARD its saved reply cap (an exit fault is never
 * replied to; the delete leaves the reply slot empty for the slot's next tenant), and mark a pending
 * exit so the next pal_guest_next reports event 0 + tears the objects down. */
int pal_guest_exit(pal_pid_t who, int code) {
    guest_t *g = gfind(who);
    if (!g) return -1;
    seL4_TCB_Suspend(g->proc.thread.tcb.cptr);
    if (g->have_reply) {
        seL4_CNode_Delete(g->reply_path.root, g->reply_path.capPtr, g->reply_path.capDepth);
        g->have_reply = 0;
    }
    g->exit_code = code & 0xff;
    g->pending_exit = 1;
    return 0;
}

/* --- guest memory: the seL4 answer to process_vm_readv (vspace_access_page_with_callback) ---------- */
struct copy_ctx { char *buf; uint64_t gaddr; size_t n; int is_write; };

static int copy_page_cb(void *access_addr, void *root_vaddr, void *cookie) {
    struct copy_ctx *c = (struct copy_ctx *)cookie;
    size_t pgoff = (uintptr_t)access_addr & (PAGE_SZ - 1);   /* callback vaddr is page-aligned */
    char *guest = (char *)root_vaddr + pgoff;
    if (c->is_write) memcpy(guest, c->buf, c->n); else memcpy(c->buf, guest, c->n);
    return 0;
}

static size_t guest_copy(guest_t *g, uint64_t gaddr, void *buf, size_t len, int is_write) {
    size_t done = 0;
    while (done < len) {
        uint64_t a = gaddr + done;
        size_t pgoff = (size_t)(a & (PAGE_SZ - 1));
        size_t n = PAGE_SZ - pgoff;
        if (n > len - done) n = len - done;
        struct copy_ctx c = { (char *)buf + done, a, n, is_write };
        int r = vspace_access_page_with_callback(&g->proc.vspace, &vspace, (void *)a,
                                                 seL4_PageBits, seL4_AllRights, 1 /* cacheable */,
                                                 copy_page_cb, &c);
        if (r) break;   /* unmapped guest page / error -- stop (short copy) */
        done += n;
    }
    return done;
}

size_t pal_guest_read (pal_pid_t who, uint64_t gaddr, void *dst, size_t len) {
    guest_t *g = gfind(who);
    return g ? guest_copy(g, gaddr, dst, len, 0) : 0;
}
size_t pal_guest_write(pal_pid_t who, uint64_t gaddr, const void *src, size_t len) {
    guest_t *g = gfind(who);
    return g ? guest_copy(g, gaddr, (void *)src, len, 1) : 0;
}

/* mmap: allocate `len` bytes of fresh anonymous memory contiguously in the GUEST's VSpace at a
 * kernel-chosen vaddr (the AIOS ABI has no MAP_FIXED / addr hint). vspace_new_pages retypes
 * Untyped -> Frames + maps them (kernel-zeroed); the Untyped pool IS the memory budget. The region
 * is RECORDED so fork can replicate it, and the vaddr is STASHED for the kernel's following
 * pal_guest_resume (case b) to reply into x0. */
uint64_t pal_guest_mmap(pal_pid_t who, size_t len) {
    guest_t *g = gfind(who);
    if (!g) return 0;
    g->inject_x0 = 0; g->have_inject = 1;   /* default failure (x0=0); the resume replies with this */
    if (len == 0) return 0;
    if (g->nmm >= MAX_MMAPS) { con_puts("[pal_sel4] mmap: region table full\n"); return 0; }
    size_t pages = (len + PAGE_SZ - 1) >> seL4_PageBits;
    void *v = vspace_new_pages(&g->proc.vspace, seL4_AllRights, pages, seL4_PageBits);
    if (!v) return 0;
    g->mm_vaddr[g->nmm] = (uint64_t)(uintptr_t)v;
    g->mm_pages[g->nmm] = pages;
    g->nmm++;
    g->inject_x0 = (uint64_t)(uintptr_t)v;
    return g->inject_x0;
}

/* ---- fork (Phase C.2): the eager VSpace copy ------------------------------------------------------ */

/* Copy [lo,hi) (page-aligned) from the parent's address space into the child's EXISTING pages via a
 * one-page bounce buffer (two double-maps per page -- the proven pal_guest_read/write path; fork is
 * not hot). Returns 0 iff every page copied whole. */
static int fork_copy_range(guest_t *gp, guest_t *gc, uint64_t lo, uint64_t hi) {
    static char bounce[PAGE_SZ];   /* single-threaded root task -- one static bounce is safe */
    for (uint64_t a = lo; a < hi; a += PAGE_SZ) {
        if (guest_copy(gp, a, bounce, PAGE_SZ, 0) != PAGE_SZ) return -1;
        if (guest_copy(gc, a, bounce, PAGE_SZ, 1) != PAGE_SZ) return -1;
    }
    return 0;
}

/* Fork `parent` (fault-stopped at its AIOS_SYS_FORK svc): build a child from the SAME CPIO ELF, copy
 * the diverged memory (writable segments, stack, mmap regions), write the child's registers as the
 * parent's full saved context with x0=0 / pc=FaultIP+4, and stash the parent's return (the child
 * pid). The kernel then resumes BOTH: the parent via the case-(b) inject reply, the child via the
 * case-(a) Inactive kick. On failure the parent's stash is -1 (fork returns -1) and every
 * half-built child object is unwound. */
pal_pid_t pal_guest_fork(pal_pid_t parent) {
    guest_t *gp = gfind(parent);
    if (!gp || !gp->have_reply) {                /* not fault-stopped -> contract violation */
        if (gp) { gp->inject_x0 = (uint64_t)-1; gp->have_inject = 1; }
        return PAL_PID_NONE;
    }
    gp->inject_x0 = (uint64_t)-1; gp->have_inject = 1;   /* default: fork failed */

    guest_t *gc = NULL;
    for (int i = 0; i < MAX_GUESTS; i++) if (!g_g[i].used) { gc = &g_g[i]; break; }
    if (!gc) { con_puts("[pal_sel4] fork: guest table full\n"); return PAL_PID_NONE; }

    if (guest_build(gc, gp->image)) return PAL_PID_NONE;

    /* The child's ELF/stack placement reproduces the parent's only by allocation-order determinism
     * (identical config -> identical first-fit); VERIFY before copying by address. */
    if (gc->stack_lo != gp->stack_lo || gc->stack_hi != gp->stack_hi || gc->nwseg != gp->nwseg) {
        con_puts("[pal_sel4] fork: child layout diverged from the parent (stack ");
        con_put_hex(gc->stack_lo); con_puts(" vs "); con_put_hex(gp->stack_lo); con_puts(")\n");
        guest_destroy_objects(gc);
        return PAL_PID_NONE;
    }

    int ok = 1;
    for (int i = 0; ok && i < gp->nwseg; i++)            /* writable ELF ranges (data + bss) */
        if (fork_copy_range(gp, gc, gp->wseg_lo[i], gp->wseg_hi[i])) ok = 0;
    if (ok && fork_copy_range(gp, gc, gp->stack_lo, gp->stack_hi)) ok = 0;   /* the whole stack */
    for (int i = 0; ok && i < gp->nmm; i++) {            /* replicate + copy the mmap regions */
        uint64_t va = gp->mm_vaddr[i]; size_t pages = gp->mm_pages[i];
        reservation_t res = vspace_reserve_range_at(&gc->proc.vspace, (void *)(uintptr_t)va,
                                                    pages * PAGE_SZ, seL4_AllRights, 1);
        if (res.res == NULL) { ok = 0; break; }
        if (vspace_new_pages_at_vaddr(&gc->proc.vspace, (void *)(uintptr_t)va, pages,
                                      seL4_PageBits, res)) { ok = 0; break; }
        gc->mm_vaddr[gc->nmm] = va; gc->mm_pages[gc->nmm] = pages; gc->nmm++;
        if (fork_copy_range(gp, gc, va, va + pages * PAGE_SZ)) { ok = 0; break; }
    }

    /* The child's registers: the parent's FULL saved context (ReadRegisters is legal on a
     * fault-blocked thread; the fault message lacks x8..x30) with fork's child-side return. */
    seL4_UserContext ctx;
    memset(&ctx, 0, sizeof ctx);
    if (ok && seL4_TCB_ReadRegisters(gp->proc.thread.tcb.cptr, 0, 0,
                                     sizeof ctx / sizeof(seL4_Word), &ctx)) ok = 0;
    if (ok) {
        ctx.x0 = 0;                                                    /* fork returns 0 in the child */
        ctx.pc = gp->fregs[seL4_UnknownSyscall_FaultIP] + 4;           /* past the svc (load-bearing) */
        if (seL4_TCB_WriteRegisters(gc->proc.thread.tcb.cptr, 0 /* stay Inactive */, 0,
                                    sizeof ctx / sizeof(seL4_Word), &ctx)) ok = 0;
    }

    if (!ok) {
        con_puts("[pal_sel4] fork: copy/registers failed -- unwinding the child\n");
        guest_destroy_objects(gc);
        return PAL_PID_NONE;
    }

    gc->used = 1;
    gc->pid  = g_next_pid++;
    g_nused++;
    /* wseg/stack metadata was rebuilt by guest_build (same ELF -> same list); started=0 makes the
     * kernel's pal_guest_resume(child) the same Inactive kick a fresh spawn gets. */
    gp->inject_x0 = (uint64_t)gc->pid;                   /* fork returns the child pid in the parent */
    return gc->pid;
}

/* ================================ stubs (made real in their phase) ================================= */
int      pal_take_term_signal(void) { return 0; }
int      pal_guest_deliver(pal_pid_t w, uint64_t h, uint64_t sg, uint64_t t, void *sv) { (void)w;(void)h;(void)sg;(void)t;(void)sv; return -1; }
int      pal_guest_sigreturn(pal_pid_t w, const void *sv) { (void)w;(void)sv; return -1; }
/* exec: still a stub (Phase C.3, needs the C.4 tarfs). Stashes x0 = -ENOSYS so that IF a guest
 * reaches it, the kernel's following pal_guest_resume (case b) returns a clean error. */
int      pal_guest_exec(pal_pid_t w, const char *p, uint64_t a, uint64_t e) {
    (void)p;(void)a;(void)e;
    guest_t *g = gfind(w);
    if (g) { g->inject_x0 = (uint64_t)(-AIOS_ENOSYS); g->have_inject = 1; }
    return -1;
}
int      pal_host_pipe(pal_file_t *rd, pal_file_t *wr) { (void)rd;(void)wr; return -1; }
/* Phase D (family C -- fs server) */
pal_file_t pal_host_open(const char *p, uint64_t fl, uint64_t m) { (void)p;(void)fl;(void)m; return (pal_file_t)-AIOS_ENOSYS; }
long     pal_host_read (pal_file_t f, void *b, size_t n) { (void)f;(void)b;(void)n; return -AIOS_ENOSYS; }
int      pal_host_close(pal_file_t f) { (void)f; return 0; }
long long pal_host_lseek(pal_file_t f, long long o, int w) { (void)f;(void)o;(void)w; return -AIOS_ENOSYS; }
int      pal_host_fstat(pal_file_t f, struct aios_stat *o) { (void)f;(void)o; return -AIOS_ENOSYS; }
long     pal_host_getdents(pal_file_t f, void *b, size_t n) { (void)f;(void)b;(void)n; return -AIOS_ENOSYS; }
int      pal_host_stat  (const char *p, struct aios_stat *o, int fo) { (void)p;(void)o;(void)fo; return -AIOS_ENOSYS; }
int      pal_host_unlink(const char *p) { (void)p; return -AIOS_ENOSYS; }
int      pal_host_mkdir (const char *p, unsigned int m) { (void)p;(void)m; return -AIOS_ENOSYS; }
int      pal_host_rmdir (const char *p) { (void)p; return -AIOS_ENOSYS; }
int      pal_host_rename(const char *o, const char *n) { (void)o;(void)n; return -AIOS_ENOSYS; }
int      pal_host_chdir (const char *p) { (void)p; return -AIOS_ENOSYS; }
pal_file_t pal_host_openat   (pal_file_t d, const char *p, uint64_t fl, uint64_t m) { (void)d;(void)p;(void)fl;(void)m; return (pal_file_t)-AIOS_ENOSYS; }
int      pal_host_fstatat  (pal_file_t d, const char *p, struct aios_stat *o, int fo) { (void)d;(void)p;(void)o;(void)fo; return -AIOS_ENOSYS; }
int      pal_host_unlinkat (pal_file_t d, const char *p, int rd) { (void)d;(void)p;(void)rd; return -AIOS_ENOSYS; }
int      pal_host_faccessat(pal_file_t d, const char *p, int am) { (void)d;(void)p;(void)am; return -AIOS_ENOSYS; }
long     pal_host_readlink (const char *p, char *b, size_t n) { (void)p;(void)b;(void)n; return -AIOS_ENOSYS; }
int      pal_host_fchmodat (pal_file_t d, const char *p, unsigned int m, int nf) { (void)d;(void)p;(void)m;(void)nf; return -AIOS_ENOSYS; }
int      pal_host_fchownat (pal_file_t d, const char *p, unsigned int o, unsigned int g, int nf) { (void)d;(void)p;(void)o;(void)g;(void)nf; return -AIOS_ENOSYS; }
int      pal_host_symlinkat(const char *t, pal_file_t d, const char *l) { (void)t;(void)d;(void)l; return -AIOS_ENOSYS; }
int      pal_host_linkat   (pal_file_t od, const char *op, pal_file_t nd, const char *np, int fo) { (void)od;(void)op;(void)nd;(void)np;(void)fo; return -AIOS_ENOSYS; }
int      pal_host_utimensat(pal_file_t d, const char *p, const struct aios_timespec *t, int nf) { (void)d;(void)p;(void)t;(void)nf; return -AIOS_ENOSYS; }
/* Phase E (family C -- console/termios) */
int      pal_host_isatty(pal_file_t f) { (void)f; return 0; }
int      pal_host_tcgetattr(pal_file_t f, struct aios_termios *o) { (void)f;(void)o; return -AIOS_ENOSYS; }
int      pal_host_tcsetattr(pal_file_t f, int a, const struct aios_termios *i) { (void)f;(void)a;(void)i; return -AIOS_ENOSYS; }
int      pal_host_clock_gettime(int id, struct aios_timespec *o) { (void)id;(void)o; return -AIOS_ENOSYS; }
/* Phase F (family C -- net server) */
pal_file_t pal_host_socket (int d, int t, int p) { (void)d;(void)t;(void)p; return (pal_file_t)-AIOS_ENOSYS; }
int      pal_host_connect  (pal_file_t f, const void *a, unsigned int l) { (void)f;(void)a;(void)l; return -AIOS_ENOSYS; }
int      pal_host_bind     (pal_file_t f, const void *a, unsigned int l) { (void)f;(void)a;(void)l; return -AIOS_ENOSYS; }
int      pal_host_listen   (pal_file_t f, int b) { (void)f;(void)b; return -AIOS_ENOSYS; }
pal_file_t pal_host_accept (pal_file_t f, void *a, unsigned int *l) { (void)f;(void)a;(void)l; return (pal_file_t)-AIOS_ENOSYS; }
int      pal_host_setsockopt (pal_file_t f, int lv, int on, const void *ov, unsigned int ol) { (void)f;(void)lv;(void)on;(void)ov;(void)ol; return -AIOS_ENOSYS; }
int      pal_host_getsockname(pal_file_t f, void *a, unsigned int *l) { (void)f;(void)a;(void)l; return -AIOS_ENOSYS; }
int      pal_host_sock_error (pal_file_t f) { (void)f; return -AIOS_ENOSYS; }
int      pal_host_sock_writable(pal_file_t f) { (void)f; return -AIOS_ENOSYS; }
void     pal_net_watch_reset(void) { }
void     pal_net_watch_add  (pal_file_t f, int w) { (void)f;(void)w; }
void     pal_net_watch_timeout(int ms) { (void)ms; }
int      pal_net_have_watches(void) { return 0; }
int      pal_net_wait_ready (void) { return 0; }

/* ================================ the root task entry ============================================== */
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    con_puts("\n[pal_sel4] AIOS userspace kernel -- Phase C.2: the process table + fork\n");
    con_puts("[pal_sel4]   (badged fault EPs, SaveCaller reply tokens, eager VSpace copy, teardown).\n");

    /* Run the host-agnostic kernel. It spawns the init guest, resumes it, then services trapped AIOS
     * syscalls from EVERY guest -- now including FORK and the parked-parent WAIT -- until none remain. */
    char *gargv[] = { (char *)"aios-uk", (char *)"/sbin/init", 0 };
    int code = aios_kernel_main(2, gargv);

    con_puts("[pal_sel4] Phase C.2 complete: guests forked, waited, exited on seL4; init exit code=");
    con_put_int(code);
    con_puts(".\n[pal_sel4] halting.\n");
    seL4_TCB_Suspend(seL4_CapInitThreadTCB);
    for (;;) { }
    return code;
}

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
 * Phase C.2: the PROCESS TABLE + pal_guest_fork. Guests live in guest_t slots, each with a BADGED
 *   copy (badge = slot+1) of the ONE master fault EP -- the badge of the fault-handler cap installed
 *   in the faulter's cspace is delivered with every fault (kernel sendFaultIPC passes capEPBadge;
 *   sel4utils COPIES our pre-badged cap into the child, preserving it). Because non-MCS Recv DELETES
 *   an unconsumed caller cap, every fault's implicit reply cap is immediately seL4_CNode_SaveCaller'd
 *   into a per-guest slot; replies go out via seL4_Send on the saved slot, which funnels into the
 *   SAME doReplyTransfer as an in-place seL4_Reply and consumes the cap. That is what lets a parent
 *   PARK in wait() across other guests' faults and still be woken later. FORK is an EAGER copy:
 *   configure a fresh process from the SAME CPIO ELF (deterministic layout, VERIFIED against the
 *   parent before any copy-by-address), then overwrite what diverged -- the writable PT_LOAD ranges
 *   (parsed from the ELF's own phdrs; sel4utils' preload path throws its region records away), the
 *   whole eagerly-mapped stack, and the parent's mmap regions (replicated at IDENTICAL vaddrs).
 *   The child's registers are the parent's FULL 36-word seL4_UserContext with x0=0 and pc=FaultIP+4
 *   (the +4 is load-bearing: an unadvanced pc re-runs the svc). Object TEARDOWN: a reported exit
 *   destroys the process + our badged EP copy + the reply slot.
 * Phase C.3 (THIS): pal_guest_exec -- the kernel IS the ELF loader. Two mechanisms:
 *   (1) The HAND-STAGED SysV STACK replaces sel4utils_spawn_process_v for BOTH spawn and exec: the
 *       AIOS guest ABI is [sp]=argc, [sp+8]=argv[0].., NULL, envp.., NULL, strings (libaios _start
 *       does `ldr x0,[sp]; add x1,sp,#8` and environ = argv+argc+1) -- which is NOT the layout
 *       sel4utils stages, so the loader writes the block itself at a 16-aligned sp near stack_top
 *       (via the proven guest-copy path) and starts the image with WriteRegisters(pc=entry_point,
 *       sp) -- the same configure+WriteRegisters+Resume shape fork already proved.
 *   (2) The DOUBLE-BUFFERED image swap: sel4utils_process_t embeds its vspace bookkeeping INLINE
 *       (vspace.data points at process->data), so a configured process can NEVER be struct-copied.
 *       Each slot therefore holds proc[2]; exec builds the new image into proc[cur^1] while the old
 *       image stays fault-stopped and INTACT, and only after the new image is fully staged does it
 *       destroy proc[cur] and flip cur -- a failed exec (bad path, E2BIG, allocator) leaves the
 *       caller running exactly as POSIX requires (the Linux PAL restores staged-over bytes; here
 *       nothing was staged over). argv/envp are read out of the OLD image first (guest pointers),
 *       the path is basename-mapped onto the CPIO (the tarfs is C.4; -ENOENT for unknown images),
 *       and per-slot state resets: image meta committed, mmap list cleared, started=0 so the
 *       kernel's pal_guest_resume gives the new image the same Inactive kick a fresh spawn gets.
 *
 * The design + every seL4 API used here is kernel-source-validated (the Phase B + C.2 research; all
 * claims re-verified adversarially against deps/kernel + deps/seL4_libs; C.3 adds NO new seL4 API --
 * entry_point being set on the preload path was source-verified at libsel4utils/src/process.c:551).
 * Key facts carried forward:
 *   - Fault msg (aarch64 seL4_UnknownSyscall_Msg): X0..X7 = MR0..7, FaultIP=8, SP=9, LR=10, SPSR=11,
 *     Syscall=12, Length=13.  The AIOS nr is MR7 (X7).  Args are X0..X5.
 *   - To RESUME an UnknownSyscall fault: reply label 0, length 9, MRs = the faulted frame with
 *     X0<-retval and FaultIP<-FaultIP+4. A fault reply writes at most the first 12 MRs (registers);
 *     length 9 stops before SP/LR/SPSR (the LR reply slot actually maps to ELR_EL1 -- never echo it).
 *   - seL4_CNode_SaveCaller MOVES the caller cap; the dest slot must be EMPTY (else seL4_DeleteFirst),
 *     and the no-caller case SILENTLY "succeeds" -- our invariant makes it moot: a fault was just
 *     Recv'd, so the caller cap is always present.
 *   - Guest memory (the seL4 answer to process_vm_readv): vspace_access_page_with_callback duplicates
 *     the ONE guest frame containing an address into the root vspace (the STORED frame caps are the
 *     full-rights originals, so this works for elf-loaded, stack, and new_pages pages alike) and hands
 *     the callback the mapped vaddr; cacheable=1 matches the guest ([[feedback_pipe_shm_cache]]).
 *   - WriteRegisters sanitises SPSR to EL0 PSTATE_USER (a zeroed context is safe to start an image).
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
#include <elf/elf.h>             /* fork/exec parse the guest ELF's phdrs (writable PT_LOAD ranges) */
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

/* ================================ the process table (Phase C.2/C.3) ================================ */
#define MAX_GUESTS   16          /* PAL capacity this phase (the kernel's own table holds 64) */
#define MAX_MMAPS    32          /* anonymous regions per guest (libaios malloc grows by whole mmaps) */
#define MAX_WSEGS     8          /* writable PT_LOAD ranges per ELF (static -nostdlib images have ~1) */
#define PAGE_SZ      ((size_t)BIT(seL4_PageBits))
#define X_MAX_ARGS   32          /* exec argv/envp entries (each), C-phase limits */
#define X_STR_MAX   256          /* one argv/envp string */
#define X_BUF      8192          /* the whole staged stack block (vectors + strings) */

/* The per-IMAGE metadata (what an exec replaces): the CPIO name, the writable PT_LOAD ranges fork
 * must copy, and the eagerly-mapped stack range. Filled by guest_image_build into a caller-owned
 * struct and only COMMITTED into the slot once the new image is fully live (exec atomicity). */
typedef struct img_meta {
    char       image[64];
    uint64_t   wseg_lo[MAX_WSEGS], wseg_hi[MAX_WSEGS];   /* page-aligned [lo,hi) */
    int        nwseg;
    uint64_t   stack_lo, stack_hi;                       /* [lo,hi), eagerly mapped; guard is below */
} img_meta_t;

typedef struct guest {
    int        used;
    pal_pid_t  pid;              /* MONOTONIC (g_next_pid) -- never reused, so the kernel's zombie
                                  * bookkeeping can never alias a dead pid with a fresh guest */
    /* sel4utils_process_t embeds its vspace bookkeeping INLINE (vspace.data -> &proc.data), so a
     * configured process cannot be struct-copied -- exec double-buffers instead: the live image is
     * proc[cur]; exec builds into proc[cur^1] and flips only on success. */
    sel4utils_process_t proc[2];
    int        cur;
    img_meta_t img;
    cspacepath_t fault_ep_path;  /* our badged copy of g_fault_ep (badge = slot+1); slot-lifetime */
    cspacepath_t reply_path;     /* the SaveCaller slot: this guest's in-flight reply cap */
    int        have_reply;       /* a saved, unconsumed reply cap sits in reply_path */
    seL4_Word  fregs[seL4_UnknownSyscall_Length];   /* the pending fault's register snapshot */
    int        started;          /* 0 = Inactive (spawn/fork/exec kick pending); 1 = has run */
    uint64_t   inject_x0;        /* an inject primitive's result (mmap addr, fork's child pid) ... */
    int        have_inject;      /* ... consumed by pal_guest_resume's case-(b) reply */
    int        pending_exit;     /* exit accepted (TCB suspended); report + teardown on next next() */
    int        exit_code;
    uint64_t   mm_vaddr[MAX_MMAPS]; size_t mm_pages[MAX_MMAPS];   /* live mmap regions (fork copies;
                                                                   * exec clears -- new address space) */
    int        nmm;
} guest_t;

#define GPROC(g) (&(g)->proc[(g)->cur])

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

/* Parse the image's writable PT_LOAD ranges (page-aligned) from the CPIO ELF into `meta`. This is
 * fork's copy list: sel4utils' preload path loads every segment eagerly but throws its region
 * records away (elf.c load_record_regions frees them), so the ELF's own phdrs are the only truthful
 * source of "which pages can have diverged from the file image". */
static int load_wsegs(const char *image, img_meta_t *meta) {
    unsigned long size = 0;
    const void *file = cpio_get_file(_cpio_archive,
                                     (unsigned long)(_cpio_archive_end - _cpio_archive),
                                     image, &size);
    if (!file) { con_puts("[pal_sel4] loader: CPIO lookup failed\n"); return -1; }
    elf_t elf;
    if (elf_newFile(file, size, &elf)) { con_puts("[pal_sel4] loader: bad guest ELF\n"); return -1; }
    meta->nwseg = 0;
    for (size_t i = 0; i < elf_getNumProgramHeaders(&elf); i++) {
        if (elf_getProgramHeaderType(&elf, i) != 1 /* PT_LOAD */) continue;
        if (!(elf_getProgramHeaderFlags(&elf, i) & 2 /* PF_W */)) continue;
        if (meta->nwseg >= MAX_WSEGS) { con_puts("[pal_sel4] loader: too many writable segments\n"); return -1; }
        uint64_t va = (uint64_t)elf_getProgramHeaderVaddr(&elf, i);
        uint64_t end = va + (uint64_t)elf_getProgramHeaderMemorySize(&elf, i);   /* memsz: data+bss */
        meta->wseg_lo[meta->nwseg] = va & ~((uint64_t)PAGE_SZ - 1);
        meta->wseg_hi[meta->nwseg] = (end + PAGE_SZ - 1) & ~((uint64_t)PAGE_SZ - 1);
        meta->nwseg++;
    }
    return 0;
}

/* SLOT-lifetime resources: the badged fault-EP copy (badge = slot+1 -- how pal_guest_next tells the
 * faulters apart) and the SaveCaller reply slot. Allocated once per slot tenancy (spawn/fork), kept
 * across exec (badge and reply slot are image-independent), freed at teardown. */
static int guest_slot_init(guest_t *g) {
    cspacepath_t src;
    vka_cspace_make_path(&vka, g_fault_ep.cptr, &src);
    if (vka_cspace_alloc_path(&vka, &g->fault_ep_path)) return -1;
    if (seL4_CNode_Mint(g->fault_ep_path.root, g->fault_ep_path.capPtr, g->fault_ep_path.capDepth,
                        src.root, src.capPtr, src.capDepth,
                        seL4_AllRights, (seL4_Word)(gslot(g) + 1))) {
        vka_cspace_free(&vka, g->fault_ep_path.capPtr);
        return -1;
    }
    if (vka_cspace_alloc_path(&vka, &g->reply_path)) {
        seL4_CNode_Delete(g->fault_ep_path.root, g->fault_ep_path.capPtr, g->fault_ep_path.capDepth);
        vka_cspace_free(&vka, g->fault_ep_path.capPtr);
        return -1;
    }
    return 0;
}

static void guest_slot_fini(guest_t *g) {
    seL4_CNode_Delete(g->fault_ep_path.root, g->fault_ep_path.capPtr, g->fault_ep_path.capDepth);
    vka_cspace_free(&vka, g->fault_ep_path.capPtr);
    vka_cspace_free(&vka, g->reply_path.capPtr);
}

/* IMAGE-lifetime: configure proc[which] from the CPIO `image` (preload: ELF + stack + IPC buffer
 * eagerly built) with the slot's badged fault EP, and fill `meta` (wsegs + the stack range;
 * committed by the caller only when the image goes live). */
static int guest_image_build(guest_t *g, int which, const char *image, img_meta_t *meta) {
    size_t n = 0; while (image[n] && n < sizeof meta->image - 1) { meta->image[n] = image[n]; n++; }
    meta->image[n] = '\0';

    vka_object_t badged_ep;
    memset(&badged_ep, 0, sizeof badged_ep);
    badged_ep.cptr = g->fault_ep_path.capPtr;            /* by-cptr handoff; we keep ownership
                                                          * (process_config_fault_endpoint sets
                                                          * create_fault_endpoint=false -> own_ep
                                                          * stays false -> destroy leaves it alone) */
    sel4utils_process_config_t config = process_config_new(&simple);
    config = process_config_elf(config, meta->image, true /* preload: load the ELF eagerly now */);
    config = process_config_create_cnode(config, 12);
    config = process_config_create_vspace(config, NULL, 0);
    config = process_config_auth(config, simple_get_tcb(&simple));
    config = process_config_fault_endpoint(config, badged_ep);

    int err = sel4utils_configure_process_custom(&g->proc[which], &vka, &vspace, config);
    if (err) {
        con_puts("[pal_sel4] configure_process failed: "); con_put_int(err); con_puts("\n");
        return -1;
    }
    if (load_wsegs(meta->image, meta)) {   /* the ELF just configured fine; fails only on limits */
        sel4utils_destroy_process(&g->proc[which], &vka);
        return -1;
    }
    /* stack_size is recorded in 4K pages (sel4utils thread.c); the guard page is BELOW stack_lo and
     * unmapped, so [stack_lo, stack_hi) is exactly the eagerly-mapped range */
    meta->stack_hi = (uint64_t)(uintptr_t)g->proc[which].thread.stack_top;
    meta->stack_lo = meta->stack_hi - (uint64_t)g->proc[which].thread.stack_size * PAGE_SZ;
    return 0;
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

/* --- guest memory: the seL4 answer to process_vm_readv (vspace_access_page_with_callback) ---------- */
struct copy_ctx { char *buf; uint64_t gaddr; size_t n; int is_write; };

static int copy_page_cb(void *access_addr, void *root_vaddr, void *cookie) {
    struct copy_ctx *c = (struct copy_ctx *)cookie;
    size_t pgoff = (uintptr_t)access_addr & (PAGE_SZ - 1);   /* callback vaddr is page-aligned */
    char *guest = (char *)root_vaddr + pgoff;
    if (c->is_write) memcpy(guest, c->buf, c->n); else memcpy(c->buf, guest, c->n);
    return 0;
}

static size_t guest_copy(sel4utils_process_t *p, uint64_t gaddr, void *buf, size_t len, int is_write) {
    size_t done = 0;
    while (done < len) {
        uint64_t a = gaddr + done;
        size_t pgoff = (size_t)(a & (PAGE_SZ - 1));
        size_t n = PAGE_SZ - pgoff;
        if (n > len - done) n = len - done;
        struct copy_ctx c = { (char *)buf + done, a, n, is_write };
        int r = vspace_access_page_with_callback(&p->vspace, &vspace, (void *)a,
                                                 seL4_PageBits, seL4_AllRights, 1 /* cacheable */,
                                                 copy_page_cb, &c);
        if (r) break;   /* unmapped guest page / error -- stop (short copy) */
        done += n;
    }
    return done;
}

size_t pal_guest_read (pal_pid_t who, uint64_t gaddr, void *dst, size_t len) {
    guest_t *g = gfind(who);
    return g ? guest_copy(GPROC(g), gaddr, dst, len, 0) : 0;
}
size_t pal_guest_write(pal_pid_t who, uint64_t gaddr, const void *src, size_t len) {
    guest_t *g = gfind(who);
    return g ? guest_copy(GPROC(g), gaddr, (void *)src, len, 1) : 0;
}

/* ---- the loader (Phase C.3): the hand-staged SysV stack + the register kick ---------------------- */

/* Stage the AIOS guest ABI's initial stack into proc `p`: [argc][argv ptrs][NULL][envp ptrs][NULL]
 * [strings], written as ONE block at a 16-aligned sp just below stack_top (libaios _start does
 * `ldr x0,[sp]; add x1,sp,#8`, and environ = argv+argc+1 -- sel4utils' own spawn stages a different
 * layout, so it is not used). Returns 0 (+*sp_out) or a negative AIOS errno. */
static long stage_stack(sel4utils_process_t *p, char *const argv[], int argc,
                        char *const envp[], int envc, uint64_t *sp_out) {
    static char blk[X_BUF];      /* single-threaded root task -- one static staging block */
    if (argc > X_MAX_ARGS || envc > X_MAX_ARGS) return -AIOS_E2BIG;
    size_t vec_words = 1 + (size_t)argc + 1 + (size_t)envc + 1;
    size_t off = vec_words * 8;                       /* strings start after the vectors */
    size_t soff[2 * X_MAX_ARGS];
    for (int i = 0; i < argc + envc; i++) {
        const char *s = (i < argc) ? argv[i] : envp[i - argc];
        size_t l = 0; while (s[l]) l++;
        if (off + l + 1 > sizeof blk) return -AIOS_E2BIG;
        memcpy(blk + off, s, l + 1);
        soff[i] = off;
        off += l + 1;
    }
    size_t total = (off + 15) & ~(size_t)15;
    memset(blk + off, 0, total - off);   /* zero the alignment padding: blk is shared across guests,
                                          * and stale tail bytes would leak one guest's argv/env
                                          * fragments into another's stack (the C.3 review's catch) */
    uint64_t sp = ((uint64_t)(uintptr_t)p->thread.stack_top - total) & ~(uint64_t)15;
    uint64_t *vec = (uint64_t *)blk;
    vec[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) vec[1 + i] = sp + soff[i];
    vec[1 + argc] = 0;
    for (int j = 0; j < envc; j++) vec[2 + argc + j] = sp + soff[argc + j];
    vec[2 + argc + envc] = 0;
    if (guest_copy(p, sp, blk, total, 1) != total) return -AIOS_EFAULT;
    *sp_out = sp;
    return 0;
}

/* Write the fresh image's entry registers (everything zero but pc/sp -- like execve; the kernel
 * sanitises SPSR to EL0) and leave the thread Inactive for the case-(a) resume kick. */
static int start_image(sel4utils_process_t *p, uint64_t sp) {
    seL4_UserContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.pc = (seL4_Word)(uintptr_t)p->entry_point;
    ctx.sp = sp;
    return seL4_TCB_WriteRegisters(p->thread.tcb.cptr, 0 /* stay Inactive */, 0,
                                   sizeof ctx / sizeof(seL4_Word), &ctx);
}

/* ================================ the trap loop (families A+B) ===================================== */

/* spawn: load the C.3 test guest from the CPIO into a fresh table slot, stage its initial stack from
 * the kernel-provided argv, and leave it Inactive so the kernel registers it before it runs
 * (pal_guest_resume then kicks it). Still ignores `path` -- the init guest is hardcoded until the
 * C.4 tarfs gives paths meaning. */
pal_pid_t pal_guest_spawn(const char *path, char *const argv[]) {
    if (boot_once()) return PAL_PID_NONE;
    if (!g_have_ep) {
        if (vka_alloc_endpoint(&vka, &g_fault_ep)) { con_puts("[pal_sel4] fault-ep alloc failed\n"); return PAL_PID_NONE; }
        g_have_ep = 1;
    }
    guest_t *g = NULL;
    for (int i = 0; i < MAX_GUESTS; i++) if (!g_g[i].used) { g = &g_g[i]; break; }
    if (!g) { con_puts("[pal_sel4] spawn: guest table full\n"); return PAL_PID_NONE; }

    con_puts("[pal_sel4] Phase C.3: loading guest 'guest_exec' from the CPIO (kernel asked for ");
    con_puts(path); con_puts(")\n");

    memset(g, 0, sizeof *g);
    if (guest_slot_init(g)) return PAL_PID_NONE;
    if (guest_image_build(g, 0, "guest_exec", &g->img)) { guest_slot_fini(g); return PAL_PID_NONE; }

    int argc = 0; while (argv && argv[argc]) argc++;
    uint64_t sp = 0;
    if (stage_stack(&g->proc[0], argv, argc, NULL, 0, &sp) ||
        start_image(&g->proc[0], sp)) {
        con_puts("[pal_sel4] spawn: stack/registers staging failed\n");
        sel4utils_destroy_process(&g->proc[0], &vka);
        guest_slot_fini(g);
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

/* resume: (a) the spawn/fork/exec kick -- start an Inactive image (a fresh spawn or exec'd image at
 * its staged entry, a fork child at its pre-written parent context; seL4_TCB_Resume runs an Inactive
 * thread at whatever pc its registers hold) -- or (b) resume a guest stopped at a serviced syscall,
 * past an INJECT primitive (mmap/fork stash their own x0) or a setret interpose (stashed into
 * fregs[X0]). */
int pal_guest_resume(pal_pid_t who) {
    guest_t *g = gfind(who);
    if (!g) return -1;
    if (!g->started) {                             /* (a) first run: Inactive -> running */
        g->started = 1;
        seL4_TCB_Resume(GPROC(g)->thread.tcb.cptr);
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
            sel4utils_destroy_process(GPROC(g), &vka);
            guest_slot_fini(g);
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
         * reply was consumed (Send) or discarded (exit/exec's Delete). */
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
        seL4_TCB_Suspend(GPROC(g)->thread.tcb.cptr);
        seL4_CNode_Delete(g->reply_path.root, g->reply_path.capPtr, g->reply_path.capDepth);
        g->have_reply = 0;
        *who = g->pid; *exit_code = 139;           /* 128 + SIGSEGV-ish */
        sel4utils_destroy_process(GPROC(g), &vka);
        guest_slot_fini(g);
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
    seL4_TCB_Suspend(GPROC(g)->thread.tcb.cptr);
    if (g->have_reply) {
        seL4_CNode_Delete(g->reply_path.root, g->reply_path.capPtr, g->reply_path.capDepth);
        g->have_reply = 0;
    }
    g->exit_code = code & 0xff;
    g->pending_exit = 1;
    return 0;
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
    void *v = vspace_new_pages(&GPROC(g)->vspace, seL4_AllRights, pages, seL4_PageBits);
    if (!v) return 0;
    g->mm_vaddr[g->nmm] = (uint64_t)(uintptr_t)v;
    g->mm_pages[g->nmm] = pages;
    g->nmm++;
    g->inject_x0 = (uint64_t)(uintptr_t)v;
    return g->inject_x0;
}

/* ---- fork (Phase C.2): the eager VSpace copy ------------------------------------------------------ */

/* Copy [lo,hi) (page-aligned) from the parent's address space into the child's EXISTING pages via a
 * one-page bounce buffer (two double-maps per page -- the proven guest-copy path; fork is not hot).
 * Returns 0 iff every page copied whole. */
static int fork_copy_range(sel4utils_process_t *pp, sel4utils_process_t *cp, uint64_t lo, uint64_t hi) {
    static char bounce[PAGE_SZ];   /* single-threaded root task -- one static bounce is safe */
    for (uint64_t a = lo; a < hi; a += PAGE_SZ) {
        if (guest_copy(pp, a, bounce, PAGE_SZ, 0) != PAGE_SZ) return -1;
        if (guest_copy(cp, a, bounce, PAGE_SZ, 1) != PAGE_SZ) return -1;
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

    memset(gc, 0, sizeof *gc);
    if (guest_slot_init(gc)) return PAL_PID_NONE;
    if (guest_image_build(gc, 0, gp->img.image, &gc->img)) { guest_slot_fini(gc); return PAL_PID_NONE; }

    /* The child's ELF/stack placement reproduces the parent's only by allocation-order determinism
     * (identical config -> identical first-fit); VERIFY before copying by address. */
    int ok = 1;
    if (gc->img.stack_lo != gp->img.stack_lo || gc->img.stack_hi != gp->img.stack_hi ||
        gc->img.nwseg != gp->img.nwseg) {
        con_puts("[pal_sel4] fork: child layout diverged from the parent (stack ");
        con_put_hex(gc->img.stack_lo); con_puts(" vs "); con_put_hex(gp->img.stack_lo); con_puts(")\n");
        ok = 0;
    }

    sel4utils_process_t *pp = GPROC(gp), *cp = &gc->proc[0];
    for (int i = 0; ok && i < gp->img.nwseg; i++)        /* writable ELF ranges (data + bss) */
        if (fork_copy_range(pp, cp, gp->img.wseg_lo[i], gp->img.wseg_hi[i])) ok = 0;
    if (ok && fork_copy_range(pp, cp, gp->img.stack_lo, gp->img.stack_hi)) ok = 0;   /* the whole stack */
    for (int i = 0; ok && i < gp->nmm; i++) {            /* replicate + copy the mmap regions */
        uint64_t va = gp->mm_vaddr[i]; size_t pages = gp->mm_pages[i];
        reservation_t res = vspace_reserve_range_at(&cp->vspace, (void *)(uintptr_t)va,
                                                    pages * PAGE_SZ, seL4_AllRights, 1);
        if (res.res == NULL) { ok = 0; break; }
        if (vspace_new_pages_at_vaddr(&cp->vspace, (void *)(uintptr_t)va, pages,
                                      seL4_PageBits, res)) { ok = 0; break; }
        gc->mm_vaddr[gc->nmm] = va; gc->mm_pages[gc->nmm] = pages; gc->nmm++;
        if (fork_copy_range(pp, cp, va, va + pages * PAGE_SZ)) { ok = 0; break; }
    }

    /* The child's registers: the parent's FULL saved context (ReadRegisters is legal on a
     * fault-blocked thread; the fault message lacks x8..x30) with fork's child-side return. */
    seL4_UserContext ctx;
    memset(&ctx, 0, sizeof ctx);
    if (ok && seL4_TCB_ReadRegisters(pp->thread.tcb.cptr, 0, 0,
                                     sizeof ctx / sizeof(seL4_Word), &ctx)) ok = 0;
    if (ok) {
        ctx.x0 = 0;                                                    /* fork returns 0 in the child */
        ctx.pc = gp->fregs[seL4_UnknownSyscall_FaultIP] + 4;           /* past the svc (load-bearing) */
        if (seL4_TCB_WriteRegisters(cp->thread.tcb.cptr, 0 /* stay Inactive */, 0,
                                    sizeof ctx / sizeof(seL4_Word), &ctx)) ok = 0;
    }

    if (!ok) {
        con_puts("[pal_sel4] fork: copy/registers failed -- unwinding the child\n");
        sel4utils_destroy_process(&gc->proc[0], &vka);
        guest_slot_fini(gc);
        return PAL_PID_NONE;
    }

    gc->used = 1;
    gc->pid  = g_next_pid++;
    g_nused++;
    /* started=0 makes the kernel's pal_guest_resume(child) the same Inactive kick a fresh spawn gets */
    gp->inject_x0 = (uint64_t)gc->pid;                   /* fork returns the child pid in the parent */
    return gc->pid;
}

/* ---- exec (Phase C.3): the atomic image swap ------------------------------------------------------ */

/* Read a NULL-terminated guest pointer vector (argv/envp) + its strings out of image `p` into the
 * caller's pool. gvec==0 -> zero entries (legal: exec with no envp). Returns the count, or a
 * negative AIOS errno (-EFAULT unreadable, -E2BIG over the C-phase limits). */
static long read_gvec(sel4utils_process_t *p, uint64_t gvec, char *pool, size_t pool_sz,
                      char *out[], int max) {
    if (gvec == 0) return 0;
    int n = 0;
    size_t used = 0;
    for (;;) {
        uint64_t gptr = 0;
        if (guest_copy(p, gvec + (uint64_t)n * 8, &gptr, 8, 0) != 8) return -AIOS_EFAULT;
        if (gptr == 0) return n;
        if (n >= max) return -AIOS_E2BIG;
        char tmp[X_STR_MAX];
        size_t got = guest_copy(p, gptr, tmp, sizeof tmp, 0);
        size_t l = 0; while (l < got && tmp[l]) l++;
        if (l == got) return (got == sizeof tmp) ? -AIOS_E2BIG : -AIOS_EFAULT;   /* no NUL found */
        if (used + l + 1 > pool_sz) return -AIOS_E2BIG;
        memcpy(pool + used, tmp, l + 1);
        out[n++] = pool + used;
        used += l + 1;
    }
}

/* Replace guest `who`'s image with the AIOS program at `abspath` (already kernel-resolved). The
 * kernel IS the loader here: argv/envp are read out of the OLD image, the path's basename is looked
 * up in the CPIO (the C.4 tarfs will give paths real meaning), and the new image is built into the
 * slot's OTHER proc buffer -- the old image stays fault-stopped and INTACT until the new one is
 * fully staged, so a failed exec returns -errno to a still-running caller (POSIX). On success the
 * old process is destroyed, cur flips, the mmap list clears (a new address space), and started=0
 * hands the kernel's pal_guest_resume the same Inactive kick a fresh spawn gets. */
int pal_guest_exec(pal_pid_t who, const char *abspath, uint64_t gargv, uint64_t genvp) {
    guest_t *g = gfind(who);
    if (!g) return -1;
    g->inject_x0 = (uint64_t)(-AIOS_ENOENT); g->have_inject = 1;   /* default failure errno */
    if (!g->have_reply) return -1;               /* not fault-stopped -> contract violation */

    /* the CPIO has flat basenames until the C.4 tarfs: "/bin/guest_execd" -> "guest_execd" */
    const char *base = abspath;
    for (const char *s = abspath; *s; s++) if (*s == '/') base = s + 1;
    size_t blen = 0; while (base[blen]) blen++;
    if (blen >= sizeof g->img.image) {           /* the lookup + the build must agree on ONE string:
                                                  * guest_image_build truncates to 63 chars, so an
                                                  * overlong name is rejected up front, never checked
                                                  * long + built truncated (the C.3 review's catch) */
        g->inject_x0 = (uint64_t)(-AIOS_ENAMETOOLONG);
        return -1;
    }
    unsigned long fsz = 0;
    if (base[0] == '\0' ||
        !cpio_get_file(_cpio_archive, (unsigned long)(_cpio_archive_end - _cpio_archive), base, &fsz))
        return -1;                               /* -ENOENT (the default stash) */

    /* read argv/envp from the OLD image before anything else can fail expensively */
    static char apool[X_BUF], epool[X_BUF];
    char *xargv[X_MAX_ARGS], *xenvp[X_MAX_ARGS];
    sel4utils_process_t *oldp = GPROC(g);
    long argc = read_gvec(oldp, gargv, apool, sizeof apool, xargv, X_MAX_ARGS);
    long envc = (argc >= 0) ? read_gvec(oldp, genvp, epool, sizeof epool, xenvp, X_MAX_ARGS) : 0;
    if (argc < 0 || envc < 0) {
        g->inject_x0 = (uint64_t)(argc < 0 ? argc : envc);
        return -1;
    }
    if (argc == 0) { xargv[0] = (char *)abspath; argc = 1; }   /* POSIX-ish: argv[0] defaults to the path */

    /* build the NEW image into the other proc buffer; the old image is untouched until success */
    int nw = g->cur ^ 1;
    img_meta_t meta;
    memset(&meta, 0, sizeof meta);
    if (guest_image_build(g, nw, base, &meta)) {
        g->inject_x0 = (uint64_t)(-AIOS_ENOMEM);             /* lookup passed; this is resources */
        return -1;
    }
    uint64_t sp = 0;
    long serr = stage_stack(&g->proc[nw], xargv, (int)argc, xenvp, (int)envc, &sp);
    if (serr == 0 && start_image(&g->proc[nw], sp)) serr = -AIOS_ENOMEM;
    if (serr) {
        sel4utils_destroy_process(&g->proc[nw], &vka);
        g->inject_x0 = (uint64_t)serr;
        return -1;
    }

    /* the new image is live -- point of no return: retire the old one */
    seL4_TCB_Suspend(oldp->thread.tcb.cptr);
    seL4_CNode_Delete(g->reply_path.root, g->reply_path.capPtr, g->reply_path.capDepth);
    g->have_reply = 0;                           /* the exec fault is never replied to */
    sel4utils_destroy_process(oldp, &vka);
    g->cur = nw;
    g->img = meta;
    g->nmm = 0;                                  /* a fresh address space: old mmaps died with oldp */
    g->started = 0;                              /* the kernel's resume gives the Inactive kick */
    g->have_inject = 0;
    return 0;
}

/* ================================ stubs (made real in their phase) ================================= */
int      pal_take_term_signal(void) { return 0; }
int      pal_guest_deliver(pal_pid_t w, uint64_t h, uint64_t sg, uint64_t t, void *sv) { (void)w;(void)h;(void)sg;(void)t;(void)sv; return -1; }
int      pal_guest_sigreturn(pal_pid_t w, const void *sv) { (void)w;(void)sv; return -1; }
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
    con_puts("\n[pal_sel4] AIOS userspace kernel -- Phase C.3: exec (the kernel IS the ELF loader)\n");
    con_puts("[pal_sel4]   (the hand-staged SysV stack + the double-buffered atomic image swap).\n");

    /* Run the host-agnostic kernel. It spawns the init guest, resumes it, then services trapped AIOS
     * syscalls from EVERY guest -- now including EXEC -- until none remain. */
    char *gargv[] = { (char *)"aios-uk", (char *)"/sbin/init", 0 };
    int code = aios_kernel_main(2, gargv);

    con_puts("[pal_sel4] Phase C.3 complete: guests forked, exec'd, exited on seL4; init exit code=");
    con_put_int(code);
    con_puts(".\n[pal_sel4] halting.\n");
    seL4_TCB_Suspend(seL4_CapInitThreadTCB);
    for (;;) { }
    return code;
}

/*
 * uk/pal/sel4/boot.c -- the REAL seL4 PAL backend. It boots the host-agnostic AIOS kernel
 * (uk/kernel/aios_kernel.c, compiled -Dmain=aios_kernel_main) as a seL4 ROOT TASK and services its
 * guests. It grows phase by phase (docs/PLAN_20260709_sel4_real_port.md).
 *
 * Phase A: console (pal_host_write -> seL4_DebugPutChar) + a root-task main(); pal_guest_spawn refused.
 * Phase B: FAMILY A -- the fault-EP trap loop. A guest is a seL4 thread; its AIOS `svc`
 *   UnknownSyscall-FAULTS to the root task (seL4/aarch64 reads the syscall nr from x7, which Phase 0
 *   pinned to the AIOS number -- x7 >= 0x1000 is never a valid (negative) seL4 syscall, so every AIOS
 *   svc deterministically faults). The root seL4_Recv()s the fault, decodes it (the 13-MR message
 *   carries X0..X7/FaultIP/...), the host-agnostic kernel services it (WRITE -> the console; the
 *   buffer is read from the guest's own frames), and pal_guest_return REPLIES to resume the guest
 *   (X0=retval, FaultIP+=4 to step past the svc). guest_hello (embedded in the image's CPIO) thus
 *   runs to completion: WRITE its banner, then EXIT. This is the crux new work of the port.
 * Phase C.1 (THIS): FAMILY B begins -- pal_guest_mmap (vspace_new_pages: Untyped->Frames mapped into the
 *   guest VSpace, the proven 0.4.x MMAP_ANON pattern) + the case-(b) RESUME-PAST-SYSCALL reply. An inject
 *   primitive stashes its result (the mmap addr) and the kernel then calls pal_guest_resume, which now
 *   replies FaultIP+4 with that stashed x0 (a fault-stopped guest resumes via a reply, NOT seL4_TCB_
 *   Resume, which would re-run the svc) -- the Phase B review fix. So a guest that mmaps runs; the loaded
 *   test guest is guest_mmap (mmap a region, write+read a pattern in the fresh memory, exit 42 on match).
 *
 * The design + every seL4 API used here is kernel-source-validated (the Phase B research). Key facts:
 *   - Fault msg (aarch64 seL4_UnknownSyscall_Msg): X0..X7 = MR0..7, FaultIP=8, SP=9, LR=10, SPSR=11,
 *     Syscall=12, Length=13.  The AIOS nr is MR7 (X7).  Args are X0..X5.
 *   - To RESUME an UnknownSyscall fault: reply with label 0 (a nonzero label leaves the guest Inactive)
 *     and length >= 9, MRs = the faulted register frame with X0<-retval and FaultIP<-FaultIP+4. The
 *     kernel restarts the thread at register[FaultIP] (NOT LR), so FaultIP+4 lands past the svc; a
 *     length-0 reply re-runs the svc -> infinite fault loop. Registers beyond the reply length keep
 *     their faulted values, so SP/LR/SPSR need not be echoed (the svc changed none of them).
 *   - Non-MCS reply cap: the implicit one-slot caller cap from the fault Recv backs seL4_Reply, valid
 *     until the next Recv/Call on this thread. Servicing WRITE/EXIT here is self-contained -- the
 *     console (seL4_DebugPutChar) and guest-memory copy (Page_Map inside vspace_access_page_with_
 *     callback) are pure KERNEL-OBJECT invocations, which do NOT clobber the caller cap; only a
 *     userspace-ENDPOINT seL4_Call would (that arrives in Phase D/F with the fs/net servers, where
 *     seL4_CNode_SaveCaller + a deferred reply becomes necessary -- see [[feedback_sel4_nested_call]]).
 *   - Guest memory (the seL4 answer to process_vm_readv): vspace_access_page_with_callback duplicates
 *     the ONE guest frame containing an address into the root vspace and hands the callback the mapped
 *     vaddr; map cacheable=1 to match the guest (a cacheable mismatch reads stale data on real A-profile
 *     silicon -- [[feedback_pipe_shm_cache]]).
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
#include <string.h>              /* memcpy -- muslc (the root task links the C runtime) */

#include "pal.h"                 /* the contract (pulls in aios_abi.h) -- boot.c uses no AIOS version
                                  * macros; the kernel (aios_kernel.c) prints the version banner. */

/* aios_kernel.c's main(), renamed by -Dmain=aios_kernel_main (build plumbing; source byte-identical). */
int aios_kernel_main(int argc, char **argv);

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

/* ================================ the guest (Phase B: exactly one) ================================= */
/* Phase B runs a single guest (guest_hello). Its fault registers are snapshotted on each fault so the
 * reply can rebuild the register frame (the fault msg + the reply share the IPC buffer, so we must not
 * rely on the buffer surviving the service path). Phase C generalises this to a process table. */
static sel4utils_process_t g_proc;
static vka_object_t        g_fault_ep;
static int                 g_have_guest = 0;
static const pal_pid_t     GUEST_PID    = 1;
static seL4_Word           g_fregs[seL4_UnknownSyscall_Length];   /* the pending fault's 13 registers */
static int                 g_pending_exit = 0;
static int                 g_exit_code    = 0;
static int                 g_started      = 0;   /* 0 = Inactive (needs the spawn-kick); 1 = running */
static uint64_t            g_inject_x0    = 0;   /* an inject primitive's result (mmap addr, ...) ... */
static int                 g_have_inject  = 0;   /* ... consumed by pal_guest_resume's case-(b) reply */

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

/* ================================ Phase B: family A -- the trap loop =============================== */

/* spawn: load "guest_mmap" (the Phase C.1 test guest) from the CPIO, build its VSpace/TCB/CSpace with a
 * fault endpoint pointing at us, and leave it SUSPENDED (resume=0) so the kernel registers it before it
 * runs toward its first svc (pal_guest_resume then starts it). Phase C.1 still ignores `path` -- the only
 * guest is the hardcoded guest_mmap; the real path-resolving loader is Phase C.3/D (exec + the fs server). */
pal_pid_t pal_guest_spawn(const char *path, char *const argv[]) {
    (void)argv;
    if (boot_once()) return PAL_PID_NONE;
    if (g_have_guest) { con_puts("[pal_sel4] Phase B services a single guest\n"); return PAL_PID_NONE; }

    con_puts("[pal_sel4] Phase C: loading guest 'guest_mmap' from the CPIO (kernel asked for ");
    con_puts(path); con_puts(")\n");

    if (vka_alloc_endpoint(&vka, &g_fault_ep)) { con_puts("[pal_sel4] fault-ep alloc failed\n"); return PAL_PID_NONE; }

    sel4utils_process_config_t config = process_config_new(&simple);
    config = process_config_elf(config, "guest_mmap", true);
    config = process_config_create_cnode(config, 12);
    config = process_config_create_vspace(config, NULL, 0);
    config = process_config_auth(config, simple_get_tcb(&simple));
    config = process_config_fault_endpoint(config, g_fault_ep);   /* by value (vka_object_t) */

    int err = sel4utils_configure_process_custom(&g_proc, &vka, &vspace, config);
    if (err) { con_puts("[pal_sel4] configure_process failed: "); con_put_int(err); con_puts("\n"); return PAL_PID_NONE; }

    err = sel4utils_spawn_process_v(&g_proc, &vka, &vspace, 0, NULL, 0 /* resume=0: leave suspended */);
    if (err) { con_puts("[pal_sel4] spawn_process failed: "); con_put_int(err); con_puts("\n"); return PAL_PID_NONE; }

    g_have_guest = 1;
    return GUEST_PID;
}

/* reply_resume: resume a FAULT-STOPPED guest past its serviced svc -- reply with label 0, length 9,
 * x0 = the result, X1..X7 echoed from the fault snapshot, FaultIP+4 (step past the svc). Shared by
 * pal_guest_return (x0 = the syscall retval) and pal_guest_resume case (b) (x0 = an inject primitive's
 * stashed result). Rebuilds every MR from the g_fregs snapshot because the service path (e.g.
 * vspace_new_pages) used the IPC buffer; the non-MCS caller cap survives because only KERNEL-OBJECT
 * invocations ran between the fault Recv and here (Retype/Page_Map -- never a userspace-endpoint Call;
 * the Phase B guest_copy Page_Map already proved this). */
static void reply_resume(uint64_t x0) {
    seL4_SetMR(seL4_UnknownSyscall_X0, x0);
    for (int i = seL4_UnknownSyscall_X1; i <= seL4_UnknownSyscall_X7; i++) seL4_SetMR(i, g_fregs[i]);
    seL4_SetMR(seL4_UnknownSyscall_FaultIP, g_fregs[seL4_UnknownSyscall_FaultIP] + 4);   /* past the svc */
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, seL4_UnknownSyscall_FaultIP + 1 /* length 9 */));
}

/* resume: pal.h uses this for TWO things -- (a) the initial spawn-kick (start a freshly-loaded, Inactive
 * guest at its ELF entry) and (b) resume a guest STOPPED at a serviced syscall, past an INJECT primitive
 * (mmap/exec/fork, which stash their own x0 in g_inject_x0) or a setret/signal interpose. Phase C
 * implements BOTH: (a) seL4_TCB_Resume the Inactive thread; (b) reply FaultIP+4 with the stashed x0 (a
 * fault-stopped guest's FaultIP still points AT the svc, so seL4_TCB_Resume would re-run it + re-fault).
 * This fixes the contract gap the Phase B review flagged. */
int pal_guest_resume(pal_pid_t who) {
    (void)who;
    if (!g_have_guest) return -1;
    if (!g_started) {                              /* (a) first run: Inactive -> running */
        g_started = 1;
        seL4_TCB_Resume(g_proc.thread.tcb.cptr);
        return 0;
    }
    uint64_t x0 = g_have_inject ? g_inject_x0 : g_fregs[seL4_UnknownSyscall_X0];  /* (b) past the svc */
    g_have_inject = 0;
    reply_resume(x0);
    return 0;
}

/* next: report a pending exit (event 0), else block on the fault EP for the guest's next syscall
 * (event 1) or death (event 0). One seL4_Recv is the whole event source (the seL4 analogue of the
 * ptrace waitpid loop). */
int pal_guest_next(pal_pid_t *who, pal_syscall_t *sc, int *exit_code) {
    if (g_pending_exit) {
        *who = GUEST_PID; *exit_code = g_exit_code;
        g_pending_exit = 0; g_have_guest = 0;
        return 0;
    }
    if (!g_have_guest) return -1;

    seL4_Word badge = 0;
    seL4_MessageInfo_t tag = seL4_Recv(g_fault_ep.cptr, &badge);

    if (seL4_isUnknownSyscall_tag(tag)) {
        for (int i = 0; i < seL4_UnknownSyscall_Length; i++) g_fregs[i] = seL4_GetMR(i);
        *who = GUEST_PID;
        sc->nr     = g_fregs[seL4_UnknownSyscall_X7];   /* the AIOS nr (Phase 0 pinned x7) */
        sc->arg[0] = g_fregs[seL4_UnknownSyscall_X0];
        sc->arg[1] = g_fregs[seL4_UnknownSyscall_X1];
        sc->arg[2] = g_fregs[seL4_UnknownSyscall_X2];
        sc->arg[3] = g_fregs[seL4_UnknownSyscall_X3];
        sc->arg[4] = g_fregs[seL4_UnknownSyscall_X4];
        sc->arg[5] = g_fregs[seL4_UnknownSyscall_X5];
        return 1;
    }

    /* Any other fault on the single guest (a real VM fault, a cap fault, ...) is unexpected in Phase B
     * -- report it as a death so the kernel loop terminates cleanly rather than hanging. */
    con_puts("[pal_sel4] guest fault (non-syscall) label="); con_put_int((long)seL4_MessageInfo_get_label(tag));
    con_puts(" -- terminating guest\n");
    seL4_TCB_Suspend(g_proc.thread.tcb.cptr);
    *who = GUEST_PID; *exit_code = 139;   /* 128 + SIGSEGV-ish */
    g_have_guest = 0;
    return 0;
}

/* return: set the guest's syscall result and resume it past the svc (a fault-stopped guest). */
int pal_guest_return(pal_pid_t who, uint64_t retval) {
    (void)who;
    reply_resume(retval);
    return 0;
}

/* setret: set the result but do NOT resume (the kernel interposes signal delivery: setret, then
 * deliver/sigreturn or resume). That signal interpose is Phase E and is UNREACHABLE in Phase C.1
 * (guest_mmap raises no signals; pal_guest_deliver/sigreturn are stubs). The case-(b) resume mechanism
 * now EXISTS (pal_guest_resume, C.1), but it replies with an inject primitive's g_inject_x0, not this
 * setret snapshot -- so this stashed x0 is NOT consumed until the Phase-E signal path wires setret ->
 * resume. Kept as a documented placeholder, not a working interpose. */
int pal_guest_setret(pal_pid_t who, uint64_t retval) {
    (void)who;
    g_fregs[seL4_UnknownSyscall_X0] = retval;   /* Phase E will reply FaultIP+4 with this x0 */
    return 0;
}

/* exit: terminate the guest. Suspend its TCB (do NOT reply to the EXIT fault -- a nonzero-label reply
 * would leave it Inactive anyway, but suspend is explicit) and mark a pending exit so the next
 * pal_guest_next reports event 0 to the kernel's reap bookkeeping. */
int pal_guest_exit(pal_pid_t who, int code) {
    (void)who;
    if (g_have_guest) seL4_TCB_Suspend(g_proc.thread.tcb.cptr);
    g_exit_code = code & 0xff;
    g_pending_exit = 1;
    return 0;
}

/* --- guest memory: the seL4 answer to process_vm_readv (vspace_access_page_with_callback) ---------- */
struct copy_ctx { char *buf; uint64_t gaddr; size_t n; int is_write; };

static int copy_page_cb(void *access_addr, void *root_vaddr, void *cookie) {
    struct copy_ctx *c = (struct copy_ctx *)cookie;
    size_t pgoff = (uintptr_t)access_addr & (BIT(seL4_PageBits) - 1);   /* callback vaddr is page-aligned */
    char *guest = (char *)root_vaddr + pgoff;
    if (c->is_write) memcpy(guest, c->buf, c->n); else memcpy(c->buf, guest, c->n);
    return 0;
}

static size_t guest_copy(uint64_t gaddr, void *buf, size_t len, int is_write) {
    if (!g_have_guest) return 0;
    size_t done = 0;
    while (done < len) {
        uint64_t a = gaddr + done;
        size_t pgoff = (size_t)(a & (BIT(seL4_PageBits) - 1));
        size_t n = (size_t)BIT(seL4_PageBits) - pgoff;
        if (n > len - done) n = len - done;
        struct copy_ctx c = { (char *)buf + done, a, n, is_write };
        int r = vspace_access_page_with_callback(&g_proc.vspace, &vspace, (void *)a,
                                                 seL4_PageBits, seL4_AllRights, 1 /* cacheable */,
                                                 copy_page_cb, &c);
        if (r) break;   /* unmapped guest page / error -- stop (short copy) */
        done += n;
    }
    return done;
}

size_t pal_guest_read (pal_pid_t who, uint64_t gaddr, void *dst, size_t len)       { (void)who; return guest_copy(gaddr, dst, len, 0); }
size_t pal_guest_write(pal_pid_t who, uint64_t gaddr, const void *src, size_t len) { (void)who; return guest_copy(gaddr, (void *)src, len, 1); }

/* ================================ stubs (made real in their phase) ================================= */
int      pal_take_term_signal(void) { return 0; }
int      pal_guest_deliver(pal_pid_t w, uint64_t h, uint64_t sg, uint64_t t, void *sv) { (void)w;(void)h;(void)sg;(void)t;(void)sv; return -1; }
int      pal_guest_sigreturn(pal_pid_t w, const void *sv) { (void)w;(void)sv; return -1; }
/* Phase C (family B -- the loader/pager). mmap: allocate `len` bytes of fresh anonymous memory and map
 * it contiguously into the GUEST's VSpace at a kernel-chosen vaddr (the AIOS ABI has no MAP_FIXED / addr
 * hint). vspace_new_pages retypes Untyped -> Frames + maps them into g_proc.vspace -- the proven 0.4.x
 * MMAP_ANON pattern (src/servers/pipe_server.c); the Untyped pool IS the guest's memory budget, and the
 * frames are kernel-zeroed. Returns the guest vaddr (0 on failure) and STASHES it as g_inject_x0 for the
 * kernel's following pal_guest_resume (case b) to reply into x0. seL4_AllRights matches the proven
 * pattern; a RW-only / execute-never hardening (malloc is data) is a later refinement. */
uint64_t pal_guest_mmap(pal_pid_t who, size_t len) {
    (void)who;
    g_inject_x0 = 0; g_have_inject = 1;   /* default failure (x0=0); the resume replies with this */
    if (!g_have_guest || len == 0) return 0;
    size_t pages = (len + (BIT(seL4_PageBits) - 1)) >> seL4_PageBits;
    void *v = vspace_new_pages(&g_proc.vspace, seL4_AllRights, pages, seL4_PageBits);
    g_inject_x0 = (uint64_t)(uintptr_t)v;   /* the guest vaddr, or 0 if NULL */
    return g_inject_x0;
}
/* exec/fork: still stubs (Phase C.2/C.3). They stash x0 = -ENOSYS so that IF a guest reaches them, the
 * kernel's following pal_guest_resume (case b) returns a clean error rather than echoing the faulted x0
 * (garbage). Unreachable in C.1 (guest_mmap neither forks nor execs; a 2nd guest is refused). */
int      pal_guest_exec(pal_pid_t w, const char *p, uint64_t a, uint64_t e) {
    (void)w;(void)p;(void)a;(void)e; g_inject_x0 = (uint64_t)(-AIOS_ENOSYS); g_have_inject = 1; return -1; }
pal_pid_t pal_guest_fork(pal_pid_t p) {
    (void)p; g_inject_x0 = (uint64_t)(-AIOS_ENOSYS); g_have_inject = 1; return PAL_PID_NONE; }
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
    con_puts("\n[pal_sel4] AIOS userspace kernel -- Phase C: the process model (family B), sub-milestone 1:\n");
    con_puts("[pal_sel4]   mmap (Untyped->Frames into the guest VSpace) + the resume-past-syscall reply.\n");

    /* Run the host-agnostic kernel. It spawns the guest (pal_guest_spawn), resumes it, then services its
     * faulted AIOS syscalls -- now including AIOS_SYS_MMAP -- until it EXITs, returning the exit code. */
    char *gargv[] = { (char *)"aios-uk", (char *)"/sbin/init", 0 };
    int code = aios_kernel_main(2, gargv);

    con_puts("[pal_sel4] Phase C.1 complete: guest mmap'd + ran on seL4 via the fault-EP trap loop; exit code=");
    con_put_int(code);
    con_puts(".\n[pal_sel4] halting.\n");
    seL4_TCB_Suspend(seL4_CapInitThreadTCB);
    for (;;) { }
    return code;
}

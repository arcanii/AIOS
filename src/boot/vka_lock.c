/*
 * vka_lock.c -- a thin locking layer over the root task allocman/vka.
 *
 * THE PROBLEM (v0.4.178, documented in boot_services.c): the root servers and
 * do_fork share ONE global allocman/vka with NO locking. The CSpace-slot bitmap
 * RMW in libsel4allocman (single_level.c:68-69: index=CLZL(bitmap[i]) then
 * bitmap[i] &= ~BIT(index)) tears under SMP -- two cores read the same word,
 * compute the same free-bit, both clear it, and BOTH return the same slot cptr.
 * The second mint into that slot then clobbers a saved reply cap (the
 * "second SSH connection fails / key exchange failed" bug). The interim fix was
 * to pin every root thread to core 0 so the allocator is never touched
 * concurrently; that comment notes "A finer fix would be a lock around the
 * allocator." This file is that finer fix -- the prerequisite for un-pinning the
 * servers in the symmetric-kernel redesign (docs/NEXT_20260623_symmetric_kernel_redesign.md).
 *
 * Proven by scripts/allocman_lock_host_test.c: the buggy RMW tears ~4000
 * duplicate handouts per run with 4 threads; the same RMW under this lock yields
 * zero duplicates.
 *
 * Correctness note (re-entrancy): allocman is internally re-entrant -- an alloc
 * can trigger a watermark refill that recurses into allocman_*_alloc/free. That
 * recursion goes through the allocman interfaces DIRECTLY (g_real.*), never back
 * through these locked trampolines, so it stays inside one trampoline call on
 * one thread. A plain (non-recursive) spinlock is therefore correct.
 */
#include "aios/root_shared.h"   /* externs the global vka_t vka */
#include <vka/vka.h>
#include <sel4/sel4.h>
#include <stdbool.h>
#include <stdio.h>

/* One shared spinlock -- every allocator user is a root-task thread. Same
 * primitive AIOS already uses (src/gpu/v3d.c, src/lib/posix_thread.c): a GCC
 * atomic test-and-set on a shared byte (LDAXR/STLXR on AArch64, valid across all
 * cores). Hold time is microseconds (a bitmap scan + a few words + the retype
 * syscall), and servers are mostly blocked on Recv, so contention is light.
 * The waiter yields so it does not burn a core/vCPU while spinning; the holder
 * runs in userspace (or contends the BKL fairly via the CLH queue for its retype
 * syscall), so there is no lock-vs-BKL deadlock. */
static volatile unsigned char g_vka_lock = 0;

/* Runtime A/B switch (default ON). Bypassing the lock reproduces the v0.4.178 tear once
 * the servers are distributed across cores -- the in-situ proof that THIS lock is what
 * closes it. Toggled via /proc/vkalock between A/B test runs (not mid-op). vka_unlock
 * always clears unconditionally, so a toggle that races a held section can never strand
 * the byte: worst case is a brief window with no mutual exclusion, which is the bypass we
 * asked for. */
volatile int g_vka_lock_enabled = 1;
static inline void vka_lock(void)
{
    while (g_vka_lock_enabled && __atomic_test_and_set(&g_vka_lock, __ATOMIC_ACQUIRE)) {
        seL4_Yield();
    }
}
static inline void vka_unlock(void)
{
    __atomic_clear(&g_vka_lock, __ATOMIC_RELEASE);
}

/* The real allocman vka, captured before we overwrite the global vka function
 * pointers. The trampolines pass through the caller-supplied `data` (== vka.data
 * == the allocman, unchanged) to the saved function pointers. */
static vka_t g_real;

static int lk_cspace_alloc(void *data, seL4_CPtr *res)
{
    vka_lock();
    int r = g_real.cspace_alloc(data, res);
    vka_unlock();
    return r;
}
static int lk_utspace_alloc(void *data, const cspacepath_t *dest, seL4_Word type,
                            seL4_Word size_bits, seL4_Word *res)
{
    vka_lock();
    int r = g_real.utspace_alloc(data, dest, type, size_bits, res);
    vka_unlock();
    return r;
}
static int lk_utspace_alloc_maybe_device(void *data, const cspacepath_t *dest, seL4_Word type,
                                         seL4_Word size_bits, bool can_use_dev, seL4_Word *res)
{
    vka_lock();
    int r = g_real.utspace_alloc_maybe_device(data, dest, type, size_bits, can_use_dev, res);
    vka_unlock();
    return r;
}
static int lk_utspace_alloc_at(void *data, const cspacepath_t *dest, seL4_Word type,
                               seL4_Word size_bits, uintptr_t paddr, seL4_Word *res)
{
    vka_lock();
    int r = g_real.utspace_alloc_at(data, dest, type, size_bits, paddr, res);
    vka_unlock();
    return r;
}
static void lk_cspace_free(void *data, seL4_CPtr slot)
{
    vka_lock();
    g_real.cspace_free(data, slot);
    vka_unlock();
}
static void lk_utspace_free(void *data, seL4_Word type, seL4_Word size_bits, seL4_Word target)
{
    vka_lock();
    g_real.utspace_free(data, type, size_bits, target);
    vka_unlock();
}
static uintptr_t lk_utspace_paddr(void *data, seL4_Word target, seL4_Word type, seL4_Word size_bits)
{
    vka_lock();
    uintptr_t r = g_real.utspace_paddr(data, target, type, size_bits);
    vka_unlock();
    return r;
}

/* Install over the global `vka`. Call ONCE, right after allocman_make_vka(&vka, ...),
 * before any server thread or do_fork can touch the allocator. cspace_make_path is a
 * pure computation from immutable cspace config (no shared mutable state), so it is
 * left unwrapped. */
void aios_vka_install_lock(void)
{
    g_real = vka;
    vka.cspace_alloc               = lk_cspace_alloc;
    vka.utspace_alloc              = lk_utspace_alloc;
    vka.utspace_alloc_maybe_device = lk_utspace_alloc_maybe_device;
    vka.utspace_alloc_at           = lk_utspace_alloc_at;
    vka.cspace_free                = lk_cspace_free;
    vka.utspace_free               = lk_utspace_free;
    vka.utspace_paddr              = lk_utspace_paddr;
    /* vka.data and vka.cspace_make_path unchanged */
}

/* /proc/vkalock[.0|.1] -- A/B the allocator lock at runtime. .0 bypasses it (to reproduce
 * the v0.4.178 tear with servers distributed), .1 re-enables (default). Bare = status. */
int aios_vkalock_cmd(const char *args, char *buf, int bufsize)
{
    if (args[0] == '.' && (args[1] == '0' || args[1] == '1'))
        g_vka_lock_enabled = (args[1] == '1');
    return snprintf(buf, bufsize,
        "vkalock: enabled=%d (.0 bypass the allocator lock / .1 enable; default 1)\n",
        g_vka_lock_enabled);
}

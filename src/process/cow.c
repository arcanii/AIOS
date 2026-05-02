/* AIOS process/cow.c -- copy-on-write fork support (v0.4.110).
 *
 * See include/aios/cow.h for the interface and design notes. The fault
 * handler path uses temporary mappings into the root vspace to memcpy
 * a parent frame into a freshly-allocated child frame; the child cap
 * replaces the R/O dup at the same vaddr.
 *
 * Cleanup is automatic: child's vspace tear-down with VSPACE_FREE walks
 * the page table and frees each cap. R/O dup pages use cookie=0 so the
 * underlying parent frame is NOT freed (parent retains it). Promoted
 * pages use cookie=ut so the fresh frame IS freed.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sel4/sel4.h>
#include <sel4utils/process.h>
#include <sel4utils/vspace.h>
#include <vka/capops.h>
#include <vka/object.h>
#include "aios/root_shared.h"
#include "aios/cow.h"
#include "aios/vka_audit.h"
#define LOG_MODULE "cow"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "aios/aios_log.h"

/* v0.4.122 COW Phase 2 Step 2: per-frame refcount table.
 * Observation-only: acquire/release update counts, behaviour unchanged.
 * 64 entries keeps BSS impact small (~1 KiB) -- the earlier 1024-entry
 * table shifted layout enough to amplify pre-existing BSS-fault noise. */
#define COW_FRAME_TABLE_SIZE 64

typedef struct {
    uintptr_t paddr;     /* 0 = empty slot */
    uint16_t  refcount;  /* vspaces currently sharing this frame */
    uint16_t  reserved;
} cow_frame_t;

static cow_frame_t cow_frame_table[COW_FRAME_TABLE_SIZE];

static uint32_t cow_acquires_total;
static uint32_t cow_releases_total;
static uint32_t cow_table_full_drops;     /* acquire when table full */
static uint32_t cow_release_untracked;    /* release for paddr not in table */
static uint32_t cow_unique_frames;        /* monotonic count of paddrs ever inserted */
static uint32_t cow_max_refcount_seen;
static uint32_t cow_table_live_entries;   /* active slots right now */

static cow_frame_t *cow_frame_lookup(uintptr_t paddr) {
    if (paddr == 0) return NULL;
    for (int i = 0; i < COW_FRAME_TABLE_SIZE; i++) {
        if (cow_frame_table[i].paddr == paddr) return &cow_frame_table[i];
    }
    return NULL;
}

static void cow_frame_acquire(uintptr_t paddr) {
    if (paddr == 0) return;
    cow_frame_t *f = cow_frame_lookup(paddr);
    if (f) {
        if (f->refcount < 0xFFFF) f->refcount++;
        if (f->refcount > cow_max_refcount_seen) cow_max_refcount_seen = f->refcount;
        cow_acquires_total++;
        return;
    }
    for (int i = 0; i < COW_FRAME_TABLE_SIZE; i++) {
        if (cow_frame_table[i].paddr == 0) {
            cow_frame_table[i].paddr = paddr;
            cow_frame_table[i].refcount = 1;
            cow_acquires_total++;
            cow_unique_frames++;
            cow_table_live_entries++;
            if (cow_max_refcount_seen < 1) cow_max_refcount_seen = 1;
            return;
        }
    }
    cow_table_full_drops++;
}

static void cow_frame_release(uintptr_t paddr) {
    if (paddr == 0) return;
    cow_frame_t *f = cow_frame_lookup(paddr);
    if (!f) {
        cow_release_untracked++;
        return;
    }
    if (f->refcount > 0) f->refcount--;
    cow_releases_total++;
    if (f->refcount == 0) {
        f->paddr = 0;
        if (cow_table_live_entries > 0) cow_table_live_entries--;
    }
}

static uintptr_t cow_paddr_of(seL4_CPtr cap) {
    if (cap == seL4_CapNull) return 0;
    seL4_ARM_Page_GetAddress_t r = seL4_ARM_Page_GetAddress(cap);
    if (r.error) return 0;
    return (uintptr_t)r.paddr;
}

int cow_format_stats(char *buf, int bufsize) {
    int w = 0;
    w += snprintf(buf + w, bufsize - w,
        "table_size: %d\n"
        "table_live: %u\n"
        "unique_frames: %u\n"
        "acquires: %u\n"
        "releases: %u\n"
        "release_untracked: %u\n"
        "table_full_drops: %u\n"
        "max_refcount: %u\n",
        COW_FRAME_TABLE_SIZE,
        cow_table_live_entries,
        cow_unique_frames,
        cow_acquires_total,
        cow_releases_total,
        cow_release_untracked,
        cow_table_full_drops,
        cow_max_refcount_seen);
    return w;
}

/* Note: sel4utils_elf_load (called from fork.c) goes through
 * sel4utils_elf_load_record_regions(... NULL, 0), which sets
 * clear_at_end=true and frees all per-segment reservations after
 * loading. So at cow_setup_segment time, no elf_load reservation
 * exists for the writable segment -- we must create our own.
 *
 * To avoid the v0.4.110 corruption symptom (downstream BSS faults),
 * we explicitly unmap our COW pages before sel4utils_destroy_process
 * runs. This way the entries are RESERVED (not POPULATED) when
 * sel4utils_free_reservation walks them in vspace_tear_down, so
 * clear_entries_range succeeds cleanly. cow_release_proc handles
 * this; it must be called by every code path that destroys a
 * process's vspace. */

void cow_clear_proc(int proc_idx) {
    if (proc_idx < 0 || proc_idx >= MAX_ACTIVE_PROCS) return;
    active_proc_t *ap = &active_procs[proc_idx];
    ap->num_cow_ranges = 0;
    for (int i = 0; i < MAX_COW_RANGES; i++) {
        ap->cow_starts[i] = 0;
        ap->cow_ends[i] = 0;
        ap->cow_reservations[i] = NULL;
    }
}

int cow_in_range(int proc_idx, uintptr_t addr) {
    if (proc_idx < 0 || proc_idx >= MAX_ACTIVE_PROCS) return 0;
    active_proc_t *ap = &active_procs[proc_idx];
    for (int i = 0; i < ap->num_cow_ranges; i++) {
        if (addr >= ap->cow_starts[i] && addr < ap->cow_ends[i]) return 1;
    }
    return 0;
}

static int cow_find_range(active_proc_t *ap, uintptr_t addr) {
    for (int i = 0; i < ap->num_cow_ranges; i++) {
        if (addr >= ap->cow_starts[i] && addr < ap->cow_ends[i]) return i;
    }
    return -1;
}

int cow_setup_segment(int child_idx,
                      vspace_t *parent_vs,
                      sel4utils_process_t *child_proc,
                      uintptr_t base,
                      int num_pages) {
    if (child_idx < 0 || child_idx >= MAX_ACTIVE_PROCS) return -1;
    active_proc_t *child = &active_procs[child_idx];
    if (child->num_cow_ranges >= MAX_COW_RANGES) {
        AIOS_LOG_WARN("cow_ranges full, falling back to eager copy");
        return -1;
    }
    if (num_pages <= 0) return 0;

    vspace_t *child_vs = &child_proc->vspace;
    uintptr_t end = base + (uintptr_t)num_pages * PAGE_SIZE;

    /* 1. Free child's eagerly-allocated frames in the COW range.
     *    VSPACE_FREE returns frames to vka and deletes caps. After this
     *    the entries are EMPTY (sel4utils_elf_load already freed its
     *    own reservations during loading, so no reservation covers the
     *    range to keep entries in RESERVED state). */
    vspace_unmap_pages(child_vs, (void *)base, num_pages, seL4_PageBits, &vka);

    /* 2. Reserve a fresh range with R/W rights. Entries are EMPTY
     *    after the unmap so reserve_range_at succeeds without conflict. */
    reservation_t cow_res = vspace_reserve_range_at(
        child_vs, (void *)base, (size_t)num_pages * PAGE_SIZE,
        seL4_AllRights, 1);
    if (cow_res.res == NULL) {
        AIOS_LOG_ERROR_V("cow_setup: reserve failed at base=",
                         (unsigned long)base);
        return -1;
    }

    /* 4. For each page parent has mapped, dup parent's cap R/O and map
     *    in child at the same vaddr. Pages parent doesn't have mapped
     *    (e.g., demand-paged BSS that parent never touched) are skipped;
     *    a write fault there will allocate a fresh page via the COW
     *    fault path (cow_handle_write_fault treats no-cap as zero-init). */
    int shared = 0;
    for (int i = 0; i < num_pages; i++) {
        uintptr_t va = base + (uintptr_t)i * PAGE_SIZE;
        seL4_CPtr parent_cap = vspace_get_cap(parent_vs, (void *)va);
        if (parent_cap == seL4_CapNull) continue;

        cspacepath_t src, dup;
        vka_cspace_make_path(&vka, parent_cap, &src);
        if (vka_cspace_alloc_path(&vka, &dup)) {
            AIOS_LOG_WARN_V("cow_setup: cslot alloc failed at va=", (unsigned long)va);
            continue;
        }
        vka_audit_cslot(VKA_SUB_FORK);

        seL4_CapRights_t ro = seL4_CapRights_new(0, 0, 1, 0); /* readable only */
        int cerr = seL4_CNode_Copy(dup.root, dup.capPtr, dup.capDepth,
                                   src.root, src.capPtr, src.capDepth, ro);
        if (cerr) {
            AIOS_LOG_WARN_V("cow_setup: CNode_Copy err=", (unsigned long)cerr);
            vka_cspace_free(&vka, dup.capPtr);
            continue;
        }

        /* Map the R/O dup; pass cookie=NULL so vspace_tear_down won't
         * try to free the underlying frame (parent owns it). */
        int merr = vspace_map_pages_at_vaddr(child_vs, &dup.capPtr, NULL,
            (void *)va, 1, seL4_PageBits, cow_res);
        if (merr) {
            AIOS_LOG_WARN_V("cow_setup: map err=", (unsigned long)merr);
            seL4_CNode_Delete(dup.root, dup.capPtr, dup.capDepth);
            vka_cspace_free(&vka, dup.capPtr);
            continue;
        }
        cow_frame_acquire(cow_paddr_of(parent_cap));
        shared++;
    }

    /* 5. Record the range so the fault handler can find it later. */
    int idx = child->num_cow_ranges;
    child->cow_starts[idx] = base;
    child->cow_ends[idx]   = end;
    child->cow_reservations[idx] = cow_res.res;
    child->num_cow_ranges++;

    AIOS_LOG_INFO_V("cow_setup pages_shared=", (unsigned long)shared);
    return 0;
}

int cow_handle_write_fault(int proc_idx, uintptr_t fault_addr) {
    if (proc_idx < 0 || proc_idx >= MAX_ACTIVE_PROCS) return 0;
    active_proc_t *ap = &active_procs[proc_idx];
    if (!ap->active) return 0;

    int ri = cow_find_range(ap, fault_addr);
    if (ri < 0) return 0;

    /* v0.4.121 COW Phase 2 Step 1: discriminate read vs write.
     * WnR is bit 6 of seL4_VMFault_FSR on AArch64 (1=write, 0=read).
     * R/O dups are readable, so a read fault inside a COW range means
     * an instruction fetch into data or a translation fault on a page
     * the parent never mapped. Either way, not a COW promotion -- let
     * the standard fault path handle it (kill the process). */
    seL4_Word fsr = seL4_GetMR(seL4_VMFault_FSR);
    if ((fsr & (1u << 6)) == 0) {
        AIOS_LOG_WARN_V("cow: read fault in COW range, addr=",
                        (unsigned long)fault_addr);
        return 0;
    }

    if (vka_audit_check_headroom(1) < 0) {
        AIOS_LOG_ERROR("cow fault: out of memory");
        return -1;
    }

    uintptr_t page_va = fault_addr & ~((uintptr_t)PAGE_SIZE - 1);
    vspace_t *child_vs = &ap->proc.vspace;

    /* 1. Allocate a fresh frame for the child. */
    vka_object_t frame;
    if (vka_alloc_frame(&vka, seL4_PageBits, &frame)) {
        AIOS_LOG_ERROR("cow fault: frame alloc failed");
        return -1;
    }

    /* 2. Map the new frame into the root vspace temporarily. */
    void *new_tmp = vspace_map_pages(&vspace, &frame.cptr, NULL,
        seL4_AllRights, 1, seL4_PageBits, 1);
    if (!new_tmp) {
        AIOS_LOG_ERROR("cow fault: new frame map failed");
        vka_free_object(&vka, &frame);
        return -1;
    }

    /* 3. Get the current R/O dup cap (if any) and copy bytes from it. */
    seL4_CPtr ro_cap = vspace_get_cap(child_vs, (void *)page_va);
    /* v0.4.122: capture paddr now for refcount release; once unmapped the
     * cap may be deleted and GetAddress would fail. */
    uintptr_t old_paddr = cow_paddr_of(ro_cap);
    if (ro_cap != seL4_CapNull) {
        cspacepath_t src, tmp_dup;
        vka_cspace_make_path(&vka, ro_cap, &src);
        if (vka_cspace_alloc_path(&vka, &tmp_dup) == 0) {
            int cerr = seL4_CNode_Copy(tmp_dup.root, tmp_dup.capPtr, tmp_dup.capDepth,
                                       src.root, src.capPtr, src.capDepth, seL4_AllRights);
            if (cerr == 0) {
                void *src_tmp = vspace_map_pages(&vspace, &tmp_dup.capPtr, NULL,
                    seL4_CapRights_new(0, 0, 1, 0), 1, seL4_PageBits, 1);
                if (src_tmp) {
                    memcpy(new_tmp, src_tmp, PAGE_SIZE);
                    vspace_unmap_pages(&vspace, src_tmp, 1, seL4_PageBits, NULL);
                } else {
                    AIOS_LOG_WARN("cow fault: src tmp map failed -- zero page");
                    memset(new_tmp, 0, PAGE_SIZE);
                }
                seL4_CNode_Delete(tmp_dup.root, tmp_dup.capPtr, tmp_dup.capDepth);
            } else {
                AIOS_LOG_WARN_V("cow fault: src CNode_Copy err=", (unsigned long)cerr);
                memset(new_tmp, 0, PAGE_SIZE);
            }
            vka_cspace_free(&vka, tmp_dup.capPtr);
        } else {
            AIOS_LOG_WARN("cow fault: tmp cslot alloc failed");
            memset(new_tmp, 0, PAGE_SIZE);
        }
        /* 4. Unmap and free the R/O dup from child's vspace. cookie was 0
         *    so the underlying parent frame is not freed; the dup cap and
         *    its slot are cleaned up by VSPACE_FREE. */
        vspace_unmap_pages(&vspace, new_tmp, 1, seL4_PageBits, NULL);
        vspace_unmap_pages(child_vs, (void *)page_va, 1, seL4_PageBits, &vka);
        cow_frame_release(old_paddr);
    } else {
        /* No current mapping (parent never touched this BSS page).
         * Zero the new frame -- BSS semantics. */
        memset(new_tmp, 0, PAGE_SIZE);
        vspace_unmap_pages(&vspace, new_tmp, 1, seL4_PageBits, NULL);
    }

    /* 5. Map the fresh frame at the same vaddr R/W in child. cookie=ut
     *    so vspace_tear_down(VSPACE_FREE) frees this frame on exit. */
    reservation_t cow_res = { .res = ap->cow_reservations[ri] };
    uintptr_t cookie = (uintptr_t)frame.ut;
    int merr = vspace_map_pages_at_vaddr(child_vs, &frame.cptr, &cookie,
        (void *)page_va, 1, seL4_PageBits, cow_res);
    if (merr) {
        AIOS_LOG_ERROR_V("cow fault: child map err=", (unsigned long)merr);
        vka_free_object(&vka, &frame);
        return -1;
    }

    vka_audit_frame(VKA_SUB_OTHER, 1);
    ap->audit_pages_allocated++;
    return 1;
}

/* Walk each COW range and explicitly unmap+free its pages BEFORE
 * sel4utils_destroy_process. This converts POPULATED entries (R/O
 * dups) back to RESERVED, so when vspace_tear_down later runs
 * sel4utils_free_reservation on our COW reservation, clear_entries_range
 * succeeds (it only clears RESERVED entries; hitting a POPULATED entry
 * aborts the walk and was the v0.4.110 corruption source).
 *
 * For pages that were promoted on write fault (cookie=ut, fresh frame),
 * VSPACE_FREE here properly frees the frame as well. For unpromoted
 * R/O dup pages (cookie=0), it deletes only the dup cap; the
 * underlying parent frame is untouched. */
void cow_release_proc(int proc_idx) {
    if (proc_idx < 0 || proc_idx >= MAX_ACTIVE_PROCS) return;
    active_proc_t *ap = &active_procs[proc_idx];
    if (ap->num_cow_ranges == 0) return;

    vspace_t *child_vs = &ap->proc.vspace;
    for (int i = 0; i < ap->num_cow_ranges; i++) {
        uintptr_t s = ap->cow_starts[i];
        uintptr_t e = ap->cow_ends[i];
        if (e <= s) continue;
        /* v0.4.116: walk page-by-page. vspace_unmap_pages does NOT skip
         * entries that are RESERVED but unmapped (e.g., pages parent
         * never had so cow_setup_segment did not dup). For those entries
         * get_cap returns UINTPTR_MAX, which then flows to
         * vka_cnode_delete + vka_cspace_free, silently corrupting the
         * slot allocator. We avoid that by only touching pages that
         * have a real cap. */
        for (uintptr_t va = s; va < e; va += PAGE_SIZE) {
            seL4_CPtr cap = vspace_get_cap(child_vs, (void *)va);
            if (cap == seL4_CapNull) continue;  /* EMPTY or RESERVED */
            /* v0.4.122: capture paddr before unmap; release decrements
             * the refcount if this is a tracked R/O dup. Promoted pages
             * (cookie=ut, fresh frame) were never acquired and will fall
             * through cow_frame_release as untracked. */
            uintptr_t paddr = cow_paddr_of(cap);
            vspace_unmap_pages(child_vs, (void *)va, 1, seL4_PageBits, &vka);
            cow_frame_release(paddr);
        }
    }
    /* Metadata is zeroed separately by cow_clear_proc on slot reuse. */
}

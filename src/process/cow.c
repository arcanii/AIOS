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
            vspace_unmap_pages(child_vs, (void *)va, 1, seL4_PageBits, &vka);
        }
    }
    /* Metadata is zeroed separately by cow_clear_proc on slot reuse. */
}

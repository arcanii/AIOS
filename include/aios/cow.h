#ifndef AIOS_COW_H
#define AIOS_COW_H
/*
 * cow.h -- v0.4.110 copy-on-write fork support.
 *
 * cow_setup_segment is called from do_fork() once per writable LOAD
 * segment. It releases the child's eagerly-allocated frames, replaces
 * them with R/O dups of the parent's frame caps, and records the range
 * in the child's active_proc_t. On a subsequent write fault inside the
 * range the fault handlers (in pipe_server / exec_server) call
 * cow_handle_write_fault, which allocates a fresh frame, copies bytes,
 * and remaps R/W in place.
 *
 * Phase 1: child-side stripping only. Parent keeps its R/W mapping.
 * Sufficient for the typical fork+exec pattern where the child either
 * exits or exec's before doing meaningful work.
 */
#include <stdint.h>
#include <sel4utils/process.h>
#include <sel4utils/vspace.h>

/* Replace child's eager allocation in [base, base+num_pages*4096) with
 * R/O dups of parent's caps. v0.4.124: with COW_STRIP_PARENT enabled,
 * also kernel-Page_Maps the parent R/O at the same vaddr; parent's first
 * write to each page later triggers cow_handle_write_fault on the parent
 * proc slot. Returns 0 on success, -1 on failure. */
int cow_setup_segment(int child_idx,
                      int parent_idx,
                      vspace_t *parent_vs,
                      sel4utils_process_t *child_proc,
                      uintptr_t base,
                      int num_pages);

/* Called from VM fault handlers. Returns 1 if the fault was a COW
 * write that we handled (caller should reply to resume the thread),
 * 0 if the fault is not in any COW range (caller should fall through
 * to the standard exit path), or -1 on hard error. */
int cow_handle_write_fault(int proc_idx, uintptr_t fault_addr);

/* Returns 1 if addr is in any of the proc's COW ranges. */
int cow_in_range(int proc_idx, uintptr_t addr);

/* Reset COW state for a proc slot (called when slot is reused). */
void cow_clear_proc(int proc_idx);

/* Explicitly unmap COW pages from this proc's vspace BEFORE
 * sel4utils_destroy_process is called. Required so the subsequent
 * vspace_tear_down sees the COW range entries as RESERVED (not
 * POPULATED), letting sel4utils_free_reservation walk cleanly. */
void cow_release_proc(int proc_idx);

/* v0.4.122: render the per-frame refcount-table stats into buf. */
int cow_format_stats(char *buf, int bufsize);

#endif

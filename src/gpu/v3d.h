#ifndef AIOS_GPU_V3D_H
#define AIOS_GPU_V3D_H
/*
 * v3d.h -- Broadcom V3D 4.2 GPU driver, root-side API.
 *
 * Phase 0 (power + IDENT + IRQ) exposes only init and the /proc/v3d diagnostic.
 * The render API (v3d_clear / v3d_triangle / v3d_cube_frame and the DISP_V3D_*
 * IPC) arrives in Phases 2-4 -- see docs/DESIGN_V3D_IMPLEMENTATION.md.
 */
#include <stdint.h>

/* Claim the pre-mapped V3D MMIO (hub+core, RPiVid ASB), bind the V3D IRQ fully
 * MASKED, and pin the VC mailbox tag buffer. Performs ZERO V3D register access:
 * the power-on + IDENT probe run lazily on the first /proc/v3d.power poke, never
 * in the boot path (a gated-block access can SError the A72). No-op when
 * hw_info.has_v3d == 0 (QEMU and any board without a V3D node). Must run in the
 * single-threaded boot context (IRQ binding). */
void v3d_init(void);

/* Phase 1: reserve the 8 MB phys-contiguous GPU pool (page table + scratch +
 * bump region) while low RAM is still plentiful. Call EARLY in boot, right after
 * xhci_dma_reserve and before the fs cache / display FB eat the low frames. The
 * MMU itself is programmed lazily on the first /proc/v3d.mmu | .fault poke. No-op
 * when hw_info.has_v3d == 0. Returns 0 on success (or no-GPU), -1 on alloc failure. */
int v3d_mem_reserve(void);

/* /proc/v3d diagnostic verbs. `args` is the suffix after "v3d" ("" , ".power",
 * ".power.3", ".r.08", ".clock", ...). Phase 0 = bring-up regime: every verb
 * runs DIRECT on the fs thread (nothing else touches V3D, nothing touches the
 * framebuffer). Writes the human-readable result into buf; returns its length. */
int v3d_diag_cmd(const char *args, char *buf, int bufsize);

/* Phase 2 (the GPU kick). Run a full GPU clear to the live HDMI framebuffer + a
 * center-pixel probe. MUST run on the display_server thread (the single FB writer):
 * it takes FB ownership internally (flush-all + console suspend) and, on a job
 * failure, resets the GPU and restores the console. Returns 0 on PASS, negative on
 * failure (-5 = no GPU / not powered). QEMU stub returns -5 and touches nothing. */
int v3d_clear_and_probe(uint32_t color);

/* Phase 3: render the rainbow triangle (GL Shader State path) to the live FB +
 * center/corner probe. Same thread + ownership rules as v3d_clear_and_probe.
 * Returns 0 on PASS (center pixel != clear color), negative on failure. QEMU: -5. */
int v3d_triangle_and_probe(void);

/* Phase 4a: spin a GPU-rendered shaded cube for `frames` frames (<=0 = default,
 * capped) on the display_server thread. Same thread + ownership rules. Returns 0 on
 * PASS. QEMU stub: -5. The project deliverable. */
int v3d_cube_run(int frames);

/* Diagnostic: spin a flat two-sided square (RED front / BLUE back via opposite winding)
 * for `frames` frames -- a backface-cull sanity check on a single quad. QEMU stub: -5. */
int v3d_quad_run(int frames);

/* Called by display_server when its blocking seL4_Recv wakes on the bound display
 * notification: services a pending /proc/v3d.{test,tri} request (no-op if none).
 * Mirrors the net_server bound-notification wake. */
void v3d_service_display_request(void);

#endif /* AIOS_GPU_V3D_H */

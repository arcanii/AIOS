#ifndef AIOS_GPU_V3D_REGS_H
#define AIOS_GPU_V3D_REGS_H
/*
 * v3d_regs.h -- Broadcom V3D 4.2 register offsets and bits (Phase 0 subset).
 *
 * Offsets transcribed from Linux drivers/gpu/drm/v3d/v3d_regs.h (rpi-6.6.y).
 * Phase 0 only needs the IDENT blocks (to prove the power story), the two
 * interrupt blocks (to confirm the masked IRQ counter stays 0 and to decode
 * MMU faults later), and the RPiVid ASB + PM power registers. The MMU / CLE /
 * PTB / cache-control blocks land in Phase 1+.
 *
 * Layout: the hub (0xFEC00000, 4 pages) and core0 (0xFEC04000, 4 pages) are
 * claimed as ONE contiguous 8-page region (dev_v3d_vaddr). Hub registers are at
 * dev_v3d_vaddr[off>>2]; core registers at dev_v3d_vaddr[(V3D_CORE0_OFFSET+off)>>2].
 * The RPiVid ASB power bridges are a SEPARATE page (0xFEC11000, dev_v3d_asb_vaddr).
 */

/* ---- Hub registers (relative to the hub base 0xFEC00000) ---- */
#define V3D_HUB_AXICFG        0x0000
#define V3D_HUB_UIFCFG        0x0004
#define V3D_HUB_IDENT0        0x0008   /* magic when powered; bus poison when gated */
#define V3D_HUB_IDENT1        0x000c   /* TVER[3:0] REV[7:4] NCORES[11:8] NHOSTS[15:12] */
#define V3D_HUB_IDENT2        0x0010   /* WITH_MMU = bit 8 */
#define V3D_HUB_IDENT3        0x0014

/* Hub interrupt block. NOTE: V3D MMU faults are reported HERE (hub), NOT on the
 * core CTL_INT_STS block -- check and W1C-clear BOTH blocks in Phase 1. */
#define V3D_HUB_INT_STS       0x0050
#define V3D_HUB_INT_SET       0x0054
#define V3D_HUB_INT_CLR       0x0058
#define V3D_HUB_INT_MSK_STS   0x005c
#define V3D_HUB_INT_MSK_SET   0x0060   /* write 1s here to MASK (Phase 0 keeps all masked) */
#define V3D_HUB_INT_MSK_CLR   0x0064

/* IDENT0 magics (ASCII, LSB-first), HW-VERIFIED on a real RPi4 in Phase 0
 * bring-up (v0.4.19x). The design doc was WRONG about which register holds the
 * "V3D\004" magic: the HUB IDENT0 reads "VHUB", and the canonical 0x04443356
 * ("V3D\004") lives in the CORE0 IDENT0, not the hub. Both flip from the bus
 * poison when the block powers on. */
#define V3D_HUB_IDENT0_MAGIC  0x42554856u   /* "VHUB"  -- hub powered + present */
#define V3D_CORE_IDENT0_MAGIC 0x04443356u   /* "V3D\004" -- core0 powered + V3D v4 */
/* What a powered-DOWN / clock-gated read returns on this bus (HW-confirmed).
 * Pre-power READS are safe -- the hazard is misordered/wrong WRITES. A read of
 * 0x00000000 instead means a mapping bug, not a power problem. */
#define V3D_BUS_POISON        0xDEADBEEFu

/* Hub IDENT1 field extractors. */
#define V3D_HUB_IDENT1_TVER(x)    (((x) >> 0)  & 0xf)
#define V3D_HUB_IDENT1_REV(x)     (((x) >> 4)  & 0xf)
#define V3D_HUB_IDENT1_NCORES(x)  (((x) >> 8)  & 0xf)
#define V3D_HUB_IDENT1_NHOSTS(x)  (((x) >> 12) & 0xf)
/* Hub IDENT2. */
#define V3D_HUB_IDENT2_WITH_MMU   (1u << 8)

/* Hub INT bits: V3D MMU faults arrive on this (hub) block. */
#define V3D_HUB_INT_MMU_WRV   (1u << 5)   /* write violation */
#define V3D_HUB_INT_MMU_PTI   (1u << 4)   /* page-table invalid (the Phase 1 fault probe) */
#define V3D_HUB_INT_MMU_CAP   (1u << 3)   /* cap exceeded */

/* ---- Core 0 ---- core base = hub base + 0x4000 within the 8-page region. */
#define V3D_CORE0_OFFSET      0x4000

/* Core CTL registers (relative to the core base). */
#define V3D_CTL_IDENT0        0x0000
#define V3D_CTL_IDENT1        0x0004   /* NSLC[7:4] QUPS[11:8] NTMU[15:12] NSEM[23:16] */
#define V3D_CTL_IDENT2        0x0008
#define V3D_CTL_INT_STS       0x0050
#define V3D_CTL_INT_SET       0x0054
#define V3D_CTL_INT_CLR       0x0058
#define V3D_CTL_INT_MSK_STS   0x005c
#define V3D_CTL_INT_MSK_SET   0x0060   /* write 1s to MASK */
#define V3D_CTL_INT_MSK_CLR   0x0064

/* Core CTL_IDENT1 field extractors. QPUs-per-core = NSLC * QUPS (= 8 on Pi 4). */
#define V3D_CTL_IDENT1_NSLC(x)   (((x) >> 4)  & 0xf)
#define V3D_CTL_IDENT1_QUPS(x)   (((x) >> 8)  & 0xf)
#define V3D_CTL_IDENT1_NTMU(x)   (((x) >> 12) & 0xf)

/* Core CTL interrupt bits.
 * TRAP (the #1 easy-to-swap mistake): the LOWER bit is RENDER done, not bin --
 *   V3D_CTL_INT_FRDONE  = bit 0 = render(frame)-done
 *   V3D_CTL_INT_FLDONE  = bit 1 = bin(flush list)-done
 * Keep this comment AT the define site. */
#define V3D_CTL_INT_FRDONE    (1u << 0)
#define V3D_CTL_INT_FLDONE    (1u << 1)
#define V3D_CTL_INT_OUTOMEM   (1u << 2)

/* ---- RPiVid ASB power bridges (separate page 0xFEC11000, dev_v3d_asb_vaddr) ----
 * Clear REQ_STOP (bit 0) to un-stop the bridge, then poll ACK (bit 1) clear.
 * Every ASB write is OR'd with the PM password (like the PM block). */
#define V3D_ASB_S_CTRL        0x08   /* slave  bridge control */
#define V3D_ASB_M_CTRL        0x0c   /* master bridge control */
#define V3D_ASB_REQ_STOP      (1u << 0)
#define V3D_ASB_ACK           (1u << 1)

/* ---- PM_GRAFX (in the already-mapped PM block 0xFE100000, dev_pm_vaddr) ----
 * Deassert V3DRSTN (bit 6) to release the V3D from reset. The PM password must
 * be OR'd into EVERY PM write or the write is ignored. */
#define V3D_PM_GRAFX          0x10c   /* byte offset within the PM block */
#define V3D_PM_PASSWORD       0x5a000000u
#define V3D_PM_V3DRSTN        (1u << 6)

/* ============================================================ *
 *  Phase 1 -- MMU + CLE (transcribed VERBATIM from Linux         *
 *  drivers/gpu/drm/v3d/v3d_regs.h @ rpi-6.6.y, 2026-06-11;       *
 *  NOT from memory -- the design's flagged trap). The MMU is a   *
 *  HUB block (offsets within the hub's 4 pages); the CLE is a    *
 *  CORE block (add V3D_CORE0_OFFSET).                            *
 * ============================================================ */

/* ---- MMU cache control (hub) ---- */
#define V3D_MMUC_CONTROL          0x1000
#define V3D_MMUC_CONTROL_ENABLE   (1u << 0)
#define V3D_MMUC_CONTROL_FLUSH    (1u << 1)
#define V3D_MMUC_CONTROL_FLUSHING (1u << 2)   /* poll until clear after FLUSH */
#define V3D_MMUC_CONTROL_CLEAR    (1u << 3)

/* ---- MMU control (hub) ---- */
#define V3D_MMU_CTL               0x1200
#define V3D_MMU_CTL_ENABLE                    (1u << 0)
#define V3D_MMU_CTL_TLB_CLEAR                 (1u << 2)
#define V3D_MMU_CTL_TLB_CLEARING              (1u << 7)   /* poll until clear */
#define V3D_MMU_CTL_WRITE_VIOLATION_EXCEPTION (1u << 9)
#define V3D_MMU_CTL_WRITE_VIOLATION_INT       (1u << 10)
#define V3D_MMU_CTL_WRITE_VIOLATION_ABORT     (1u << 11)
#define V3D_MMU_CTL_PT_INVALID_ENABLE         (1u << 16)  /* REQUIRED for the PTI fault probe */
#define V3D_MMU_CTL_PT_INVALID_EXCEPTION      (1u << 17)
#define V3D_MMU_CTL_PT_INVALID_INT            (1u << 18)
#define V3D_MMU_CTL_PT_INVALID_ABORT          (1u << 19)  /* abort the access (no stall) */
#define V3D_MMU_CTL_CAP_EXCEEDED_EXCEPTION    (1u << 24)
#define V3D_MMU_CTL_CAP_EXCEEDED_INT          (1u << 25)
#define V3D_MMU_CTL_CAP_EXCEEDED_ABORT        (1u << 26)

#define V3D_MMU_PT_PA_BASE        0x1204   /* write pt_pa >> V3D_MMU_PAGE_SHIFT */
#define V3D_MMU_VIO_ID            0x122c   /* faulting client id */
#define V3D_MMU_ILLEGAL_ADDR      0x1230   /* write (scratch_pa >> SHIFT) | ENABLE */
#define V3D_MMU_ILLEGAL_ADDR_ENABLE (1u << 31)
#define V3D_MMU_VIO_ADDR          0x1234   /* faulting GPU VA (the PTI probe reads this) */
#define V3D_MMU_DEBUG_INFO        0x1238

/* MMU_DEBUG_INFO fields (Linux v3d_regs.h). The GPU VA width drives the VIO_ADDR
 * decode: the faulting address is reg << (va_width - 32). HW-confirmed on bcm2711
 * V3D 4.2 -- DEBUG_INFO reads 0x550 -> VA_WIDTH field 5 -> va_width 35 -> shift 3
 * (raw 0x04000018 << 3 = 0x200000c0 -> page 0x20000000). PA_WIDTH is decoded the
 * same way. The old <<8 (page-shift-minus-4) guess was wrong. */
#define V3D_MMU_DEBUG_VERSION_MASK   0x0000000fu   /* bits [3:0]  */
#define V3D_MMU_DEBUG_VA_WIDTH_SHIFT 4             /* bits [7:4]  */
#define V3D_MMU_DEBUG_PA_WIDTH_SHIFT 8             /* bits [11:8] */
#define V3D_MMU_WIDTH_FIELD_MASK     0xfu
#define V3D_MMU_WIDTH_BASE           30u           /* width = BASE + field */

/* ---- PTE format (Linux v3d_mmu.c) ---- */
#define V3D_MMU_PAGE_SHIFT        12        /* 4 KB GPU pages */
#define V3D_PTE_VALID             (1u << 28)
#define V3D_PTE_WRITEABLE         (1u << 29)
#define V3D_PTE_SUPERPAGE         (1u << 31)

/* ---- CLE control-list executor (core; CT0 = bin, CT1 = render) ---- */
#define V3D_CLE_CT0CS             0x100
#define V3D_CLE_CT1CS             0x104     /* render thread control/status */
#define V3D_CLE_CT0CA             0x110
#define V3D_CLE_CT1CA             0x114     /* render current address (hang pinpoint) */
#define V3D_CLE_BFC               0x134     /* bin   frame count */
#define V3D_CLE_RFC               0x138     /* render frame count (job-done tick) */
#define V3D_CLE_CT0QBA            0x160
#define V3D_CLE_CT1QBA            0x164     /* render queued buffer addr (start) */
#define V3D_CLE_CT0QEA            0x168
#define V3D_CLE_CT1QEA            0x16c     /* render queued end addr -- WRITE = kick */
#define V3D_CLE_CT0QMA            0x170     /* bin tile-state addr  (Phase 2) */
#define V3D_CLE_CT0QMS            0x174     /* bin tile-state size  (Phase 2) */

/* ---- CLE: Phase 2 additions (tile-state base + return/list/prim counters).
 * Offsets transcribed VERBATIM from Linux drivers/gpu/drm/v3d/v3d_regs.h
 * (rpi-6.6.y), NOT from memory. Core block -- add V3D_CORE0_OFFSET. */
#define V3D_CLE_CT0QTS            0x15c     /* bin tile-state base (write before bin kick) */
#define V3D_CLE_CT0RA             0x118
#define V3D_CLE_CT1RA             0x11c     /* return addrs (dumped on hang) */
#define V3D_CLE_CT0LC             0x120
#define V3D_CLE_CT1LC             0x124     /* list counters (dump) */
#define V3D_CLE_CT0PC             0x128
#define V3D_CLE_CT1PC             0x12c     /* primitive-list counters (dump) */

/* ---- PTB (primitive-tile binner; binner overflow alloc on OUTOMEM) ---- */
#define V3D_PTB_BPCA              0x300     /* binner current alloc addr */
#define V3D_PTB_BPCS              0x304     /* binner current alloc size */
#define V3D_PTB_BPOA              0x308     /* binner overflow alloc addr (OUTOMEM refill) */
#define V3D_PTB_BPOS              0x30c     /* binner overflow alloc size (zero before bin) */

/* ---- core cache control (invalidate slices before each bin; flush L2T) ---- */
#define V3D_CTL_SLCACTL           0x024     /* slices cache all-invalidate */
#define V3D_CTL_L2TCACTL          0x030     /* L2T ctrl: (FLM<<SHIFT)|L2TFLS, poll L2TFLS */
#define V3D_CTL_L2TFLSTA          0x034     /* L2T flush range start (0 = full) */
#define V3D_CTL_L2TFLEND          0x038     /* L2T flush range end   (~0 = full) */
#define V3D_L2TCACTL_L2TFLS       (1u << 0) /* flush kick + busy bit (poll until clear) */
#define V3D_L2TCACTL_FLM_SHIFT    1         /* flush-mode field [2:1] */
#define V3D_L2TCACTL_FLM_FLUSH    0u
#define V3D_L2TCACTL_FLM_CLEAR    1u
#define V3D_L2TCACTL_FLM_CLEAN    2u

/* ---- GCA (global cache/shutdown; Phase 2+ reset path) ---- */
#define V3D_GCA_CACHE_CTRL        0x00c
#define V3D_GCA_CACHE_CTRL_FLUSH  (1u << 0)
#define V3D_GCA_SAFE_SHUTDOWN     0x0b0
#define V3D_GCA_SAFE_SHUTDOWN_EN  (1u << 0)

#endif /* AIOS_GPU_V3D_REGS_H */

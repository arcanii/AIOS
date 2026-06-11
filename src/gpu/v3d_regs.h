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

#endif /* AIOS_GPU_V3D_REGS_H */

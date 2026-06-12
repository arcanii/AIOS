# NEXT: FAT32 flash-over-network (kernel8 swap without card pulls) -- 2026-06-12

> **STATUS: DONE (v0.4.222-223, HW-VERIFIED incl cold boot).** Shipped:
> src/fat32.c + FS_FATSWAP, plat_blk_*_abs HAL, virtio MBR parse,
> /bin/fatswap, scripts/pi_flash.py, scripts/fatswap_qemu_test.py (46/46).
> Kernel deploys are now a network push + reboot -- no card pulls. See the
> project-fatswap memory and CHANGELOG v0.4.222/224. Design sketch below kept
> for reference. (The corruption scare during this work was a SEPARATE ext2
> builder bug, fixed v0.4.225 -- see NEXT_20260612_net_rx_corruption.md.)

Goal: replace the mount-card-on-Mac kernel deploy with: push kernel8.img over
the network -> AIOS rewrites it on the FAT32 boot partition -> reboot. The
2026-06-12 stall-hunt session burned ~18 physical card swaps; this item ends
that. It is also the long-pending "flash-over-network" roadmap entry
(project_netconsole memory).

## What exists already

- Block layer: blk_emmc.c sees the WHOLE card (it already parses the MBR:
  part1 type=0x0c FAT32-LBA at sector 2048 size 64MB; part2 ext2 at 133120).
  raw sector read/write primitives exist (used by the ext2 backend + blk_cache
  for drive 0). CMD25 multi-block writes exist (v0.4.172, HW-verified).
- Transport: netconsole PUSH (scripts/pi_filexfer.py, v0.4.165) lands files in
  the ext2 fs (~21KB/s receive-bound; a 1.5MB kernel ~= 75s -- acceptable;
  the receive-path speedup is a separate backlog item).
- Reboot: aios_system_reboot() (BCM2711 watchdog) + write-back flush.
- Deploy verification habit: sha-on-card + boot banner (the new tool should
  print the sha256 of what it wrote; scripts side compares).

## Design sketch (a `fatswap` tool/server verb)

1. New module src/fs/fat32.c (root task): read sector 2048 (BPB), validate
   FAT32 (bytes/sector 512, sectors/cluster, FAT count, root cluster).
   Minimal, READ + targeted WRITE -- not a general VFS mount.
2. Locate `KERNEL8 IMG` in the root directory (8.3 name; it is in the root
   dir on AIOSBOOT). Read its dirent: first cluster + size.
3. Rewrite path:
   a. If new size <= allocated clusters (round up to cluster size): overwrite
      the cluster chain contents in order, update dirent size, done. (Cluster
      size on a 64MB FAT32 partition is likely 512B-4KB -- check; kernel is
      ~1.5MB so the chain is hundreds-thousands of clusters.)
   b. If new size > allocated: extend the chain from the FAT free list (walk
      FAT for 0x00000000 entries), update BOTH FAT copies, then dirent.
   c. Simplest robust alternative (consider FIRST): free the old chain
      entirely and allocate a fresh contiguous-ish chain, then update dirent.
      Crash-safety: write data clusters first, FAT second, dirent LAST (a
      power cut mid-write leaves the old dirent -> old kernel still boots...
      only if the old chain was not yet freed -- so: allocate new chain,
      write data, update dirent, THEN free old chain. 64MB partition has
      room for two kernels).
4. Plumbing: a netconsole-reachable command, e.g. `fatswap /tmp/kernel8.img`
   (file previously PUSHed to ext2 via pi_filexfer) -- reads from ext2, writes
   to the FAT partition via raw blk sectors, prints sha256 + byte count.
   IMPORTANT: the FAT partition writes must BYPASS/flush blk_cache coherently
   (cache is keyed by drive+sector; same drive 0 -- writes through the cache
   are fine, just fsync/flush before reboot; the reboot path already flushes).
5. Host script: scripts/pi_flash.py = push + fatswap + sha-verify + reboot +
   wait-for-ping + banner check. One command replaces the whole card dance.

## Testing (QEMU-first per house rules)

QEMU's virtio disk is the bare ext2 image (no MBR/FAT). Options:
- Build a composite test image: MBR + FAT32 (mtools-generated, but NOTE the
  feedback_rpi4_fat32 memory: mtools FAT breaks the PI FIRMWARE -- fine for
  QEMU-only tests, NEVER for the real card) + the ext2 partition; teach the
  QEMU blk path to parse the MBR like the Pi path does (it may already --
  check blk_virtio/boot_fs_init partition handling).
- Unit-level: host-side pytest over the fat32.c logic via a C test harness, or
  a QEMU /proc verb that runs fatswap against the test image and the host
  fsck.fat/mdir verifies the result.
- HW acceptance: fatswap a kernel with a bumped version, reboot, banner shows
  the new build number; then ALSO power-cycle (cold boot) to prove the
  firmware still reads the FAT (the real risk: firmware is picky -- see
  feedback_rpi4_fat32).

## Risks / cautions

- The Pi firmware is intolerant of weird FAT layouts (feedback_rpi4_fat32):
  do NOT reformat; only rewrite kernel8.img's chain + dirent in place.
- Keep a recovery path: before first HW fatswap, keep a known-good kernel8
  copy on the Mac; a bricked FAT entry still mounts on the Mac for repair.
- Long-filename (LFN) entries: kernel8.img fits 8.3; if the dirent has LFN
  companions, leave them untouched (size/cluster live in the 8.3 entry).
- Current kernel = v0.4.221 single-core + Source-B quanta (see
  NEXT_20260612_vl805_dma_stall.md): netconsole commands can take 32s+ when
  a quantum hits with a keyboard attached -- unplug the keyboard for clean
  flash sessions, or just use generous timeouts.

## Kickoff prompt for the fresh session

"Read docs/NEXT_20260612_fat32_flash_over_network.md and the memories
project_netconsole, feedback_rpi4_fat32, feedback_kernel8_deploy_verify,
feedback_qemu_test_hygiene. Implement the fatswap path QEMU-first, then HW.
The Pi is at 192.168.0.8 (netconsole 2323), serial /dev/cu.usbserial-0001,
kernel deploys still by card until this works."

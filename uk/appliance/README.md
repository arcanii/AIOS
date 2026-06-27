# The minimal AIOS appliance — Linux boots straight into AIOS

AIOS is a **userspace kernel**: Linux is the interim *substrate*, AIOS is the kernel on top (the
2026-06-24 pivot). This directory packages that idea as a bootable appliance — a **minimal Linux**
(default 6.18 LTS) whose only job is to host AIOS, plus a **three-file initramfs**:

```
/init       a tiny static PID-1 launcher (aios_init.c): mounts /proc, gives the console a
            controlling terminal, sets AIOS_ROOT=/aiosroot, and execs the AIOS kernel
/aios-uk    the AIOS userspace kernel (statically linked, no libc in the image)
/aiosroot   the AIOS userland (dash as /bin/sh + the sbase coreutils + /etc/passwd), from mkaiosroot.sh
```

Boot it and Linux hands the machine to AIOS: a **confined** AIOS dash shell on the console, the host
filesystem unreachable (every guest path + exec is jailed to `/aiosroot` via the kernel's M4.2/M4.3
confinement). "Strictly minimal" — `CONFIG_NET`, `CONFIG_BLOCK`, drivers, modules are all off.

## Build + boot (in the aarch64 Linux env: the colima container as root, or the Pi)

```sh
sh appliance/build_appliance.sh      # static aios-uk + aios_init + AIOS root -> initramfs; fetch+build Linux
sh appliance/run_qemu.sh             # boot it under QEMU 'virt' (TCG, no KVM needed)
```

Expected: the Linux boot log, then `[aios-init] minimal Linux up; launching the AIOS userspace
kernel...`, then the `aios-uk` banner and a confined AIOS shell. `ls -l /bin` shows the AIOS userland;
`cat /etc/hostname` (a host path) is unreachable. Exit QEMU with `Ctrl-A x`.

## Knobs

| env | default | meaning |
|---|---|---|
| `KVER` | `6.18` | kernel version to fetch + build (any `6.18.x`; 6.18 is the LTS series) |
| `BASE` | `defconfig` | config base: `defconfig` (reliable boot) or `tinyconfig` (the strict-minimal aspiration; `aios.config` carries the platform essentials so it can boot too) |
| `PAL` | `linux` | which PAL `aios-uk` is built with (`linux` = ptrace, `seccomp` = SECCOMP_RET_TRACE) |
| `SKIP_KERNEL` | unset | reuse `out/Image`, just rebuild the initramfs |

## Files

- `aios_init.c` — the PID-1 launcher (host Linux code; the substrate's init, not an AIOS-ABI program).
- `aios.config` — the kernel options AIOS requires, as a `merge_config.sh` fragment.
- `build_appliance.sh` — orchestrates the whole build; outputs `out/Image` + `out/aios-initramfs.cpio.gz`.
- `run_qemu.sh` — boots `out/` under `qemu-system-aarch64 -M virt -nographic`.
- See `docs/AIOS_KERNEL_DEPENDENCIES.md` for *why* each kernel option is required — the precise host
  surface AIOS depends on (and the eventual seL4 PAL's proof obligation).

## Notes

- **Device nodes.** The initramfs is staged on a *local* fs (not the virtiofs mount) so `mknod
  /dev/console` works; it needs root (true in the container). On a non-root host, run the staging via
  `sudo` or point `CONFIG_INITRAMFS_SOURCE` at a `gen_init_cpio` spec instead.
- **`cpio`/`qemu`.** `build_appliance.sh` needs `cpio`; `run_qemu.sh` needs `qemu-system-aarch64`
  (`apt-get install -y cpio qemu-system-arm` in the gcc:13 container).
- **RPi4.** This targets QEMU `virt` (bootable + testable without hardware). For the real Pi, build with
  the bcm2711 DTB + RPi config deltas; the initramfs is identical. sched_ext (`CONFIG_SCHED_CLASS_EXT`,
  available in 6.18) is intentionally off here — a separate, non-minimal follow-on.

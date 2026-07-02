# Boots into AIOS on the RPi5 — the systemd light path

AIOS is a **userspace kernel**: Linux is the interim *substrate*, AIOS is the kernel on top (the
2026-06-24 pivot). The [QEMU appliance](../README.md) shows the *heavy* path — a minimal Linux 6.18
whose initramfs execs AIOS as its only job. This directory is the **light path** for a real, already-
running machine (the RPi5, Ubuntu 26.04): a **systemd service** that hands one **console** to AIOS at
boot, leaving the rest of the host — the boot chain, the primary getty on tty1, and **ssh** — untouched.

So the RPi5 keeps booting Ubuntu (and stays reachable over ssh), but a dedicated virtual terminal
(**tty8** by default) presents the AIOS system layer: `init → login → a user session`, respawned on
logout. Switch to it at the physical console with `Alt-F8` (or `sudo chvt 8`); log in `aios` / `aios`.

## Why the light path

- **Safe + reversible.** It never rewrites the bootloader, kernel, or initramfs. `systemctl disable
  --now aios-console` (and `rm -rf /opt/aios`) fully reverts it. ssh is never in the AIOS path, so a
  misbehaving AIOS console cannot lock you out of the machine.
- **Real hardware, real boot.** The machine genuinely *boots into* AIOS: systemd starts
  `aios-console.service` as part of `multi-user.target`, and AIOS owns that console for the life of the
  boot — the "userspace kernel over a commodity host" idea, live on the RPi5.
- **One command, byte-identical to the proven path.** The unit's `ExecStart` is
  `AIOS_ROOT=/opt/aios/aiosroot /opt/aios/aios-uk /opt/aios/aiosroot/sbin/init` — exactly what the
  HW-validated `login` gate key drives (on a pty); here systemd supplies the controlling tty the same
  way it does for `getty`/`login`, so AIOS job control (`^C`/`^Z`, `tcsetpgrp`) works on the console.

## Install

From the `uk/` tree on the RPi5, after `make all` (so `aios-uk` + `dash` + the sbase utils exist):

```sh
sudo sh appliance/rpi5/install.sh            # stage /opt/aios + install the unit (NOT enabled yet)
sudo systemctl start  aios-console           # try it transiently; view with: sudo chvt 8
sudo systemctl status aios-console           # active (running), MainPID = aios-uk
# make it permanent (boots into AIOS from every boot):
sudo systemctl enable --now aios-console
```

`install.sh` stages the AIOS root via `mkaiosroot.sh` (dash + the sbase coreutils + `/etc/{passwd,group,
shadow,inittab,profile,motd}`) and copies `aios-uk` into `/opt/aios`, then writes
`/etc/systemd/system/aios-console.service`. Choose a different console with `AIOS_TTY=ttyN` (default
`tty8`, outside systemd's autovt range 1–6 so it does not fight logind's on-demand gettys; a serial
console like `ttyAMA0` also works — set `TERM=vt220` in the unit).

## Verify (without a physical console — over ssh)

```sh
systemctl is-active aios-console                              # -> active
MP=$(systemctl show -p MainPID --value aios-console)
ps -o pid,comm,args --ppid "$MP"                              # aios-uk's child = the AIOS init guest
sudo cat /proc/$MP/environ | tr '\0' '\n' | grep AIOS_ROOT    # -> AIOS_ROOT=/opt/aios/aiosroot
```

The **interactive** proof (the actual `init → login → session → logout → respawn` on this exact command)
is the `login` gate key, green on the RPi5 under both PAL backends; and a throwaway pty run of the same
command shows the AIOS sysinit banner and `login:` prompt (see `appliance/rpi5/` validation notes).

## Revert

```sh
sudo systemctl disable --now aios-console
sudo rm -rf /opt/aios /etc/systemd/system/aios-console.service
sudo systemctl daemon-reload
```

## Notes / scope

- **Confinement.** The AIOS console is jailed to `/opt/aios/aiosroot` (`AIOS_ROOT`), so the guest shell +
  utilities see only that world, never the host filesystem (M4.2/M4.3). The host clock/network are still
  reachable through the kernel unless you also set `AIOS_NET_ALLOW` / `AIOS_NET_BIND_ALLOW` in the unit.
- **Not a poweroff proxy.** On a shared dev box a root `poweroff` inside AIOS just respawns the login
  (`Restart=always`); it deliberately does **not** power off the Ubuntu host (unlike the standalone
  appliance's PID-1, which maps the AIOS shutdown code to a real host power transition).
- **PAL.** Stages whatever `aios-uk` you built (`make PAL=linux` default, or `PAL=seccomp`).
- This is the deploy-on-real-hardware companion to the QEMU `appliance/`; the AIOS userland (`mkaiosroot.sh`)
  is identical.

# Contributing to AIOS

AIOS is a research OS exploring how far POSIX and Unix design can be carried
on the seL4 microkernel. Collaborators are welcome -- this guide is the entry
point; most of the detail lives in docs that already exist and stay current.

## Getting started

1. **Build it**: follow the [README Quick Start](README.md#quick-start).
   On Linux, `scripts/setup-linux.py` automates the toolchain, seL4 `deps/`
   clones, and required patches; on macOS see
   [docs/ENVIRONMENT_BUILD.md](docs/ENVIRONMENT_BUILD.md).
   `python3 scripts/build_apps.py` is the one-command everyday rebuild.
2. **Run it**: boot QEMU per the README, log in as `root`/`root`.
3. **Orient yourself**: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (system
   design), [HANDOVER.md](HANDOVER.md) (current state of work),
   [CHANGELOG.md](CHANGELOG.md) (history),
   [docs/LEARNINGS.md](docs/LEARNINGS.md) (hard-won seL4 lessons -- read this
   before touching IPC, capabilities, or memory management).

## Repo map

| Path | What |
|---|---|
| `src/` | Root task: servers (`src/servers/`), POSIX shim (`src/lib/`), drivers (`src/plat/`, `src/usb/`), network stack (`src/net/`), boot (`src/boot/`), SSH (`src/ssh/`), apps (`src/apps/` -- mostly disk-loaded; `tty_server`/`auth_server` ship in the boot CPIO) |
| `include/aios/` | Shared headers, IPC protocol constants, `version.h` |
| `scripts/` | Build scripts, `aios-cc` cross wrapper, QEMU test harnesses, Pi tooling |
| `disk/rootfs/` | Files installed into the ext2 disk image |
| `docs/` | Design docs (`DESIGN_*.md`), `LEARNINGS.md`, session handovers (`NEXT_*.md`) |
| `hw/rpi4/` | Real-hardware boot notes and firmware staging |

## Development workflow

The authoritative process doc is
[docs/DEVELOPMENT_PROTOCOL.md](docs/DEVELOPMENT_PROTOCOL.md). The essentials:

- **`main` is the only long-lived branch** (short-lived branches are fine,
  merged back within a session or two). Commit messages follow
  `v0.4.NNN: what changed` for version-bumping changes (see `git log`).
- **Version bumps**: `scripts/bump-patch.sh` increments
  `include/aios/version.h`.
- **Rebuild rules** (the gotchas that bite everyone):
  - Root-task / server changes: `ninja -C build-04` only.
  - App changes (`src/apps/`): ninja + rebuild the disk image
    (`scripts/mkdisk.py`, or just `build_apps.py`). Exception: the CPIO
    boot apps (`tty_server`, `auth_server`) need a full rebuild -- see the
    rebuild-rules table in
    [docs/DEVELOPMENT_PROTOCOL.md](docs/DEVELOPMENT_PROTOCOL.md).
  - POSIX shim changes (`src/lib/posix_*.c`): dash, zsh, and sshd link
    `libaios_posix.a` and are NOT rebuilt by ninja -- rerun
    `build_apps.py` (and `scripts/build_zsh.py` separately if your change
    affects zsh; `build_apps.py` does not rebuild it).
  - Shared-code changes that affect both platforms: build BOTH `build-04`
    (QEMU) and `build-rpi4` before calling it done.
- **QEMU first, hardware second**: develop and verify on QEMU, then deploy to
  the Pi (flash-free: `scripts/mkkernel8.py` for kernel/root-task swaps,
  `scripts/pi_filexfer.py` / netconsole for userspace). Some bug classes are
  HW-only (cache coherency, DMA windows, real timing) -- see
  `docs/LEARNINGS.md`.

## Code style

- `.clang-format` codifies the conventions (4-space indent, attached braces,
  `char *p`, ~100 columns). Apply it to **new code or changed lines only**
  (`clang-format-diff`) -- do not mass-reformat existing files.
- File header comment: `/* filename.c -- purpose */`. Block comments use
  `/* ... */`; significant changes carry a version note
  (`/* v0.4.NNN: why */`). Avoid apostrophes in comments.
- Small, focused source files; servers communicate over documented IPC
  protocols (labels in `include/aios/`).
- Python tooling prints explicit `OK` / `FAIL` lines.

## Testing

- QEMU harnesses live in `scripts/*_qemu_test.py` (ssh, smp, dns, netconsole,
  fb_scroll, 8x xhci, ...). Each boots QEMU headless itself -- no setup; run
  the ones touching your subsystem before committing, e.g.
  `python3 scripts/ssh_qemu_test.py`.
- New features ship with a test script following the ssh/smp harness style:
  [PASS]/[FAIL] per check, an `N/N passed` summary, nonzero exit on failure.
  (Some older harnesses print only a single verdict; new ones should not.)
- Hardware claims are labelled honestly: "QEMU-verified" until it has run on a
  real Pi, then "HW-verified".

## Adding things

- **A syscall**: implement `aios_sys_<name>()` in the right `src/lib/posix_*.c`
  module, register it in the installer table in `src/lib/aios_posix.c`
  (`muslcsys_install_syscall(__NR_<name>, ...)`), add server-side IPC if it
  needs root-task state, rebuild per the shim rule above, and add/extend a
  QEMU test. See `docs/LEARNINGS.md` section "POSIX Shim".
- **A driver**: platform code lives under `src/plat/<platform>/`; shared
  driver logic goes in `src/` proper behind a small HAL header (see
  `blk_hal.h` for the pattern). Bring it up on QEMU first if the device
  exists there; budget for HW-only surprises if not.

## AI-assisted development

This project deliberately uses AI (Claude) for code generation and review;
it is part of the research. The conventions that make that work -- handover
docs (`docs/NEXT_*.md`), `HANDOVER.md`, memory notes, verification discipline
-- are described in [docs/DEVELOPMENT_PROTOCOL.md](docs/DEVELOPMENT_PROTOCOL.md).
Human or AI, the bar is the same: build green on both targets, tests pass,
honest verification labels.

## Licensing

Contributions are accepted under the project MIT license ([LICENSE](LICENSE)).
Third-party code and binaries have their own terms -- see
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) before vendoring anything
new.

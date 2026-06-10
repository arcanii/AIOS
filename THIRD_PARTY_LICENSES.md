# Third-party code, licenses, and attribution

AIOS first-party code (`src/`, `include/`, `scripts/`, and `disk/rootfs/`
excluding the TinyCC material under `usr/src/tcc/` and `tmp/tcc2`)
is MIT-licensed -- see [LICENSE](LICENSE). The project also **commits some
third-party material in this repository**, **builds against external
third-party sources**, and **bundles third-party binaries into the boot/SD
images it produces**. This file records all three categories. The top-level
MIT license applies only to AIOS first-party code, not to the components
listed here.

## 1. Third-party material committed in this repository

| Component | License | Where | Notes |
|---|---|---|---|
| TinyCC 0.9.28rc (mob branch) | **LGPL-2.1** | `disk/rootfs/usr/src/tcc/` (full source, ~57 files) + prebuilt aarch64 binary `disk/rootfs/tmp/tcc2` | Copyright (c) 2001-2004 Fabrice Bellard and contributors. Committed so TCC can self-host on AIOS. The LGPL-2.1 text is included at `disk/rootfs/usr/src/tcc/COPYING`; the corresponding source accompanies the binary in the same tree, satisfying the LGPL source-availability requirement. |
| RPi4 device tree `bcm2711-rpi-4-b.dtb` | **GPL-2.0** | `hw/rpi4/firmware/`, `disk/rpi4-firmware-fresh/` | Compiled from Linux kernel `.dts` sources; obtained from [raspberrypi/firmware](https://github.com/raspberrypi/firmware) (stable boot/). Source: the Linux kernel tree. |
| RPi4 GPU firmware `fixup4.dat` | **Broadcom proprietary, redistributable** | `hw/rpi4/firmware/`, `disk/rpi4-firmware-fresh/` | Redistribution permitted for Raspberry Pi use per [boot/LICENCE.broadcom](https://github.com/raspberrypi/firmware/blob/master/boot/LICENCE.broadcom). `start4.elf` (same license) is downloaded at image-build time, not committed. |
| RPi4 EEPROM bootloader `pieeprom.upd` / `pieeprom.sig` | **Raspberry Pi (Trading) Ltd licence** | `disk/rpi4-eeprom/` | Per [rpi-eeprom LICENSE](https://github.com/raspberrypi/rpi-eeprom/blob/master/LICENSE): use/redistribution only on or for Raspberry Pi devices. |
| llama2.c material (historical, dead code) | MIT | `ref/dead_code/llm_server.c`, `ref/v03x/tools/tools/model.py`, `ref/v03x/tools/tools/train.py` | Port / copies of [karpathy/llama2.c](https://github.com/karpathy/llama2.c) (`run.c`, `model.py`, `train.py`). The Meta Llama 2 tokenizer files that once accompanied them (`tokenizer.bin`, `tokenizer.model`, `tokenizer.py` -- Llama 2 Community License) were removed in v0.4.187. |
| seL4_tools cmake-tool (historical copy) | BSD-2-Clause | `ref/v03x/tools/tools/seL4/cmake-tool/` (~40 files) | Copyright Data61/CSIRO; SPDX headers retained in-file. |

## 2. First-party implementations of published algorithms and interfaces

These are written for AIOS (MIT) but implement well-known public
specifications; listed for provenance honesty:

- `src/aios_auth.c` -- SHA-3-512 / Keccak-f[1600] from FIPS-202 (standard
  round constants; no third-party code copied).
- `src/crypto/crypto_chacha20.c` -- ChaCha20 per D. J. Bernstein's
  public-domain algorithm (RFC 8439).
- `src/lib/termcap.c` -- minimal tgetent/tgetstr/tgoto/tputs reimplementation
  with hardcoded VT100/xterm capabilities; no ncurses/termcap code copied.
- `include/virtio.h`, `include/aios/gpu.h` -- register/struct definitions from
  the OASIS VIRTIO specification (legacy MMIO interface for `virtio.h`;
  v1.1 for `gpu.h`).
- `src/plat/rpi4/net_genet.c` and `src/plat/rpi4/pcie_brcmstb.c` -- first-party
  drivers written **by reference to** Linux (GPL-2.0), U-Boot (GPL-2.0+), and
  Circle (GPL-3.0) drivers for register sequences and hardware behaviour. No
  literal code was copied; register layouts and init ordering are dictated by
  the hardware.
- `src/boot/boot_display_init.c` -- 8x8 console bitmap font (ASCII 32-126),
  project-drawn.

## 3. External sources required to build (cloned, not committed)

The build clones or expects these alongside the repo / under `deps/`
(gitignored); each retains its own license:

| Component | License | Used for |
|---|---|---|
| [seL4 kernel](https://github.com/seL4/seL4) | **GPL-2.0-only** (user-level code exempted per its syscall note) | The microkernel in every boot image |
| [seL4_libs](https://github.com/seL4/seL4_libs), [sel4runtime](https://github.com/seL4/sel4runtime) | BSD-2-Clause | Root task libraries |
| [util_libs](https://github.com/seL4/util_libs) | BSD-2-Clause (parts BSD-1/3-Clause, MIT, GPL-2.0, IBM-pibs) | Platform support |
| [seL4_tools](https://github.com/seL4/seL4_tools) | BSD-2-Clause (parts GPL-2.0) | elfloader (linked into boot images) + cmake-tool |
| [seL4 musllibc](https://github.com/seL4/musllibc) | MIT | libc for root task and all AIOS programs; headers + `libc.a` also staged onto the disk image for the TCC SDK |
| [sbase](https://git.suckless.org/sbase) | MIT | The 99 core utilities on the disk image |
| [dash](https://github.com/tklauser/dash) | BSD-3-Clause | The login shell |
| [zsh](https://github.com/zsh-users/zsh) | Zsh licence (MIT-like) | Interactive shell. Only `Src/` is built; the GPL-licensed `Functions/` scripts are **not** shipped. |
| [TinyCC](https://github.com/TinyCC/tinycc) | LGPL-2.1 | `tcc` compiler + `libtcc1.a` on the disk image (see section 1) |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) v3.6.3 | Apache-2.0 (of the Apache-2.0 OR GPL-2.0-or-later dual licence) | `libmbedcrypto.a` linked into `sshd` and `test_mbedtls` |
| [raspberrypi/firmware](https://github.com/raspberrypi/firmware) `start4.elf` | Broadcom proprietary, redistributable | Downloaded by `scripts/mksdcard.py` into SD images |

## 4. Obligations when distributing built images

The repository itself stays MIT + the committed items above. But a **built
`kernel8.img` / SD-card image** additionally embeds the GPL-2.0 seL4 kernel,
the elfloader, and GPL parts of util_libs, and bundles all the userland
components above. Anyone distributing such images (e.g. as release artifacts)
must:

1. provide or offer the corresponding seL4 (+ patches) source -- pointing at
   the exact upstream commits used satisfies this;
2. include the Broadcom / Raspberry Pi firmware licence notices;
3. keep this file (or equivalent attribution) alongside the image.

Until dependency commits are pinned in-repo, record the `deps/` commit hashes
with any published image.

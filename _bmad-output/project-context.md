---
project_name: 'AIOS'
user_name: 'Bryan'
date: '2026-06-25'
sections_completed: ['technology_stack']
existing_patterns_found: 12
---

# Project Context for AI Agents

_This file contains critical rules and patterns that AI agents must follow when implementing code in this project. Focus on unobvious details that agents might otherwise miss._

---

## Technology Stack & Versions

AIOS is a research operating system. It currently spans **two trees / two version lines** — this is
the single most important thing to get right (see the rules section):

| | Active line (`v0.5.x`) | Prior line (`v0.4.x`) |
|---|---|---|
| **What** | gVisor-style **userspace kernel on Linux** | from-scratch **seL4 microkernel** OS |
| **Tree** | `uk/` | repo root (`src/`, `kernel/`, `include/`, `projects/`) |
| **Status** | active; held at 0.5.x while the Linux HAL/PAL matures | preserved on `main` as record + fallback |
| **Build** | `make` / `uk/run.sh` | CMake + Ninja (`build-04`, `build-rpi4`) |
| **Version** | `uk/include/aios_version.h` | `include/aios/version.h` |

- **Languages:** C (gnu11), aarch64 assembly (inline `svc`, hand-written `_start`), Python 3
  (build/test/deploy tooling). No C++/JS/TS — ignore those badges.
- **Toolchain:** `aarch64-linux-gnu-` cross-gcc. The `uk/` tree iterates inside an **aarch64 Linux
  container (colima, `gcc:13`)** because the Mac host is darwin and cannot `ptrace` Linux; the seL4
  tree builds with the pinned seL4 ecosystem in `deps/` (see `DEPS.md`).
- **`uk/` libc:** AIOS shadow standard headers under `uk/lib/include`, compiled `-nostdinc -isystem
  $(cc -print-file-name=include)` so real C compiles unmodified against `libaios` (the AIOS-ABI C
  runtime). No host libc, no third-party libs in the userspace kernel.
- **Targets:** AArch64 — QEMU (virt) and Raspberry Pi 4. The Pi now runs stock **Linux 6.12** for the
  `uk/` line (login `pi`), and ran AIOS-on-bare-metal for the seL4 line.
- **Style is codified:** `.clang-format` (4-space, attached braces, `char *p`, ~100 cols) and
  `.editorconfig` (LF, final newline, trim trailing ws; Makefiles use tabs).

## Critical Implementation Rules

_Documented in the next step (collaborative generation)._

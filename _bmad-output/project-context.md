---
project_name: 'AIOS'
user_name: 'Bryan'
date: '2026-06-25'
sections_completed: ['technology_stack', 'architecture', 'build_validate_debug', 'code_style', 'workflow_commits', 'gotchas']
existing_patterns_found: 24
status: 'complete'
rule_count: 24
optimized_for_llm: true
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

_Rules are uk/-first (the active line). Cross-cutting conventions (style, commits, validation) apply
to both trees; seL4-only items are labelled "(seL4 line)"._

### Architecture rules (the `uk/` v0.5.x userspace kernel)

- **The kernel is host-agnostic — keep it that way.** `uk/kernel/aios_kernel.c` includes ONLY
  AIOS-owned headers (`aios_abi.h`, `aios_version.h`, `pal.h`) — never a host header (`<unistd.h>`…),
  never a host syscall. ALL host knowledge lives in exactly one file: `uk/pal/pal_linux.c`. Adding a
  host dependency to the kernel is a design violation.
- **The PAL seam (`uk/include/pal.h`) is sacred and minimal** — it is the future verified boundary.
  Every primitive added there is future proof obligation: add the fewest, narrowest primitives that
  work, and keep them implementable by a future `pal_sel4.c` (the same contract over seL4).
- **Programs see only the AIOS ABI.** Guest programs (`uk/guest/*.c`) target the AIOS ABI
  (`aios_abi.h`, syscall numbers ≥ `0x1000`) and link only `libaios` (`uk/lib/`) — never a Linux
  syscall directly.
- **ptrace driver rules (`pal_linux.c`):**
  - Classify every stop with `PTRACE_GET_SYSCALL_INFO` and resume past anything that is not a genuine
    syscall ENTRY — **never assume strict entry/exit alternation** (injected execve/clone leave stray
    exit/event stops behind).
  - **Save and restore the guest's registers around any injected syscall** (mmap/execve/clone/
    exit_group): the injection clobbers x0..x5 but the guest's `svc` wrapper expects all-but-x0
    preserved. (This exact bug SIGSEGV-looped once.)
  - aarch64: override the dispatched syscall number via `NT_ARM_SYSTEM_CALL`, not by writing x8.
- **Release a process's fds on exit** (`fd_release()` in `on_exit`). Pipe EOF depends on it — a dying
  writer's dup2'd pipe-stdout must actually close, or the downstream reader hangs forever.

### Build · validate · debug

- **`uk/` builds + runs in an aarch64 Linux container** (`uk/run.sh`: colima, `gcc:13`,
  `--cap-add=SYS_PTRACE`) — the Mac host is darwin and cannot `ptrace` Linux. `uk/run.sh` exiting
  `rc=0` is the suite gate.
- **Validate every milestone on colima AND the real Pi** (`scp -r uk pi@192.168.0.8:~/ && ssh … 'cd
  ~/uk && make && ./aios-uk <prog>'`). QEMU/colima first, hardware second.
- **Honest verification labels:** "QEMU-verified" until it has actually run on real HW, then
  "HW-verified". Never claim HW-verified from QEMU/colima alone.
- **A ptrace bug hangs SILENTLY** — bound it with an in-container `timeout N` (hang → rc=124) and
  instrument `pal_linux.c` with `fprintf(stderr, …)`. The container `sh` is **dash**: no
  `${PIPESTATUS[*]}` — capture exit via `cmd >/tmp/x 2>&1; rc=$?`.
- **(seL4 line) rebuild rules:** POSIX-shim changes (`src/lib/posix_*.c`) need dash/zsh/sshd rebuilt
  (ninja does NOT); app changes (`src/apps/`) need the disk image rebuilt; shared changes need BOTH
  `build-04` (QEMU) and `build-rpi4` built before calling it done.

### Code style & comments

- **`.clang-format` is authoritative** (4-space, attached braces, `char *p`, ~100 cols). Apply it to
  **new or changed lines only** (`clang-format-diff`) — never mass-reformat existing files.
- **File header:** `/* filename.c -- purpose */`. **Avoid apostrophes in comments.** (seL4 line)
  significant changes carry a `/* v0.4.NNN: why */` note.
- **Python tooling prints explicit `OK` / `FAIL` lines.** Test scripts print `[PASS]`/`[FAIL]` per
  check + an `N/N passed` summary, and exit nonzero on failure.
- Small, focused source files.

### Workflow & commits

- **Commit per milestone; do NOT push — Bryan pushes.** Agents commit locally only.
- **Commit messages:** descriptive on the `uk/` line; `v0.4.NNN: what changed` for version-bumping
  changes on the seL4 line. End every commit message with the `Co-Authored-By: Claude …` trailer.
- **Branches:** `main` is the only long-lived branch; the userspace-kernel work lives on the
  `userspace-kernel` branch.
- **Never commit build artifacts** — they are gitignored (`uk/.gitignore`: `/aios-uk`, `/guest_*`,
  `/prog_*`). `.claude/` and `_bmad/` are gitignored too; `_bmad-output/` is tracked.

### Critical gotchas / don't-miss

- **Don't conflate the two trees.** `uk/` (v0.5.x, Linux userspace kernel) and the repo root (v0.4.x,
  seL4) are different architectures, build systems, and version files. Always know which line a
  change belongs to before touching it.
- **Some bug classes are HW-only** (cache coherency, DMA windows, real timing) — QEMU/colima cannot
  catch them; they need the Pi (`docs/LEARNINGS.md`).
- **The ~32.4 s seL4 idle-teardown stall** is the open concern that motivated the pivot — mooted by
  leaving the platform, NOT cured. Don't reopen it as active work.
- **Read the latest handover before starting.** Session continuity rides on the handover docs
  (`docs/HANDOVER_*.md`, `docs/NEXT_*.md`) and the AI memory system — they hold state the code/git
  history doesn't.

---

## Usage Guidelines

**For AI agents:** read this before implementing; follow the rules exactly; when in doubt prefer the
more restrictive option and confirm; first establish which line — `uk/` (v0.5.x) or the seL4 tree
(v0.4.x) — the change belongs to.

**For humans:** keep it lean and uk/-first; update when the ABI / PAL surface / stack changes; prune
rules that become obvious. Authoritative deeper context:
`docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md`, the latest `docs/HANDOVER_*.md`,
`CONTRIBUTING.md`, `docs/LEARNINGS.md`.

Last updated: 2026-06-25

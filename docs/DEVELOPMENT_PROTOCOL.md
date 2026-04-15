# AIOS Development Protocol

## Branch Strategy: Single Main Branch

All development happens on `main`. Platform-specific code uses compile-time
guards (`#ifdef PLAT_RPI4`, `#ifdef PLAT_QEMU_VIRT`). Both targets build
from the same tree.

**Do not use long-lived feature branches.** They diverge, accumulate merge
debt, and duplicate work (tests, scripts, docs benefit both platforms).

### When to use a short-lived branch

- Risky or experimental changes that may be abandoned
- Multi-day refactors that would break the build on main
- Merge back within 1-2 sessions, then delete the branch

### Build both targets from main

```bash
# QEMU (default)
cd build-04 && ninja

# RPi4
cd build-rpi4 && ninja
```

The `settings.cmake` (QEMU) and `settings-rpi4.cmake` (RPi4) select the
target. The CMakeLists.txt uses `AIOS_PLATFORM` to include the correct
platform drivers.

## Version Protocol

### Version format

`v0.MINOR.PATCH` (e.g. v0.4.97)

- **MINOR** stays at 4 throughout this development cycle
- **PATCH** increments each session/milestone
- **BUILD** auto-increments on each ninja build (build_number.h)

### Bump protocol

1. Commit the current version first (via GitHub Desktop)
2. Bump for the next work phase:

```bash
./scripts/bump-patch.sh
./scripts/version.sh
```

3. Never use bump-minor unless explicitly directed

## Commit Protocol

- Commit at each milestone with version tag in message
- Format: `v0.4.XX: short description`
- Body: bullet points describing what changed and why
- Commits made via GitHub Desktop (not CLI git commit)
- Push: `git push origin main`

### Commit message style (from repo history)

```
v0.4.97: Linux build environment, test suite, QEMU validation

Linux build environment:
- setup-linux.py: automated environment setup
- build_dash.py: dash cross-compilation
- qemu-boot.py: QEMU launcher

Test suite (5 programs, all pass on QEMU):
- test_hw: hardware validation (17/17)
- test_fswrite: ext2 write/read/unlink (13/13)
```

## Session Documentation

### NEXT docs

Each development session produces a `docs/NEXT_YYYYMMDD[a-z].md` file
documenting what was implemented, key learnings, and next priorities.

The letter suffix (a, b, c) distinguishes multiple sessions on the same day.

**Structure:**

```markdown
# AIOS Next Steps -- YYYY-MM-DDx

Continuing **v0.4.XX** development.

## Session Result: [one-line summary]

## Features Implemented
### 1. Feature Name (files changed)
**Problem:** ...
**Solution:** ...

## Key Learnings
### Topic
- Detail

## Next Session Priorities
1. ...
2. ...
```

### When to archive

NEXT docs are never deleted. They form the development history.
The most recent NEXT doc is the primary reference for the next session.

## Code Change Protocol

### Source edits

- Use Python scripts for file edits (heredoc quoting is unreliable)
- Use Claude Code Edit tool when available (preferred)
- Avoid sed for multi-line edits or edits with quotes/slashes
- Always verify changes: grep for expected content after edit

### Rebuild rules

| Changed | Rebuild |
|---------|---------|
| aios_root.c, server code | `ninja` (incremental) |
| CPIO apps (tty_server, auth_server) | Full rebuild (rm -rf build dir) |
| Disk apps (mini_shell, getty, etc.) | `ninja` + `mkdisk.py` |
| sbase tools | `build_sbase.py` + `mkdisk.py` |
| dash | `build_dash.py` + `mkdisk.py` |
| POSIX shim (posix_*.c) | `ninja` + rebuild dash + `mkdisk.py` |

### Platform-specific code

Use `#ifdef` guards for platform-specific code:

```c
#ifdef PLAT_RPI4
    /* RPi4-specific: mini UART, EMMC2, GENET, VideoCore */
#endif

#ifdef PLAT_QEMU_VIRT
    /* QEMU-specific: virtio, PL011, ramfb */
#endif
```

Platform drivers live in separate directories:
- `src/plat/rpi4/` — BCM2711 drivers
- `src/plat/qemu-virt/` — virtio and QEMU device drivers

Common code uses the HAL interface (`src/plat/blk_hal.h`).

## Testing Protocol

### Test programs

Test programs live in `src/apps/test_*.c` and follow this pattern:

```c
static int tests_run = 0;
static int tests_pass = 0;

static void check(const char *name, int cond) {
    tests_run++;
    if (cond) { tests_pass++; printf("  PASS: %s\n", name); }
    else { printf("  FAIL: %s\n", name); }
}

int main(void) {
    printf("=== Test Name ===\n");
    /* ... tests ... */
    printf("\n=== Results: %d/%d passed ===\n", tests_pass, tests_run);
    return (tests_pass == tests_run) ? 0 : 1;
}
```

### Current test suite

| Test | What it validates | Expected |
|------|-------------------|----------|
| test_hw | DTB parsing, /proc, platform detection | 17/17 |
| test_fswrite | ext2 create/write/read/stat/unlink | 13/13 |
| test_smp | CPU cores, parallel fork, PID uniqueness | 4/4 |
| test_pipe_rw | pipe creation, read/write, EOF, dup2 | 11/13 |
| signal_test | sigaction, sigprocmask, kill, SIG_IGN | 16/16 |
| hwinfo | Hardware identification (not a test, diagnostic) | N/A |

### Validation before commit

1. Build succeeds: `ninja` (no errors)
2. Boot QEMU: `python3 scripts/qemu-boot.py`
3. Login works: `root` / `root`
4. Run test suite: `test_hw`, `test_fswrite`, `test_smp`
5. Verify no regressions in basic operations: `ls`, `cat /etc/passwd`, `echo hello | cat`

## Script Protocol

### Script format for AI sessions

All scripts in one code block with echo separators:

```bash
echo "=============================="
echo "Script A -- Description"
echo "=============================="
# ... commands ...
echo "=============================="
echo "End Script A"
echo "=============================="
```

### Script rules

- No non-executable characters in code blocks
- No single-quote apostrophes in C comments
- Python scripts preferred over bash for complex operations
- All scripts must be idempotent (safe to run twice)
- Include guards for double execution
- Verify changes after applying them

## Design Philosophy

- Pure POSIX: no alias tables, no prefix stripping, no magic
- Strict Unix philosophy: shell searches PATH, sends full path to exec
- Correctness over performance (research OS)
- Modular server architecture with small source files
- Everything is IPC: all syscalls route through capability-protected endpoints
- No hacks: temporary workarounds must be documented and removed

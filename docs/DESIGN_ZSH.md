# AIOS Z Shell (zsh) Port Design

## Executive Summary

Port zsh to AIOS as an alternative login shell alongside dash. zsh provides
advanced interactive features (ZLE line editor, programmable completion,
arrays, associative arrays) that dash lacks. The port is staged: Phase 1
delivers a script-mode shell (~2-3 sessions), Phase 2 adds interactive
editing (~2 sessions after termios support), Phase 3 adds job control
(longer-term). Binary size is ~4-8MB vs dash's ~250KB.

## Why zsh

| Feature | dash | zsh |
|---|---|---|
| POSIX compliance | Yes | Yes (+ extensions) |
| Line editing | None (relies on tty cooked mode) | ZLE (vi/emacs modes, history search) |
| Tab completion | None | Programmable completion system |
| Arrays | None | Indexed + associative arrays |
| Glob extensions | Basic | Extended (**/, qualifiers, ~) |
| Prompt | Static PS1 | Dynamic expansion (%~, git info, etc.) |
| Math | $((expr)) | $((expr)) + floating point |
| Functions | Basic | Autoloaded, local scope, return values |
| History | None (in-process only) | Persistent, shared, searchable |
| Binary size | ~250KB | ~4-8MB |
| SLOC | ~13K | ~70-90K |

zsh is the natural upgrade path once dash proves the POSIX infrastructure
works. The /etc/passwd pw_shell mechanism (v0.4.68) already supports
per-user shell selection -- users can choose dash or zsh.

## Prerequisites: What AIOS Needs

### Already Working (v0.4.68)

| Requirement | Status |
|---|---|
| fork + exec + waitpid | Done |
| File I/O (open, read, write, close, lseek) | Done |
| Pipes (pipe2, dup2) | Done |
| Signals (sigaction, kill, sigprocmask) | Done |
| Directory ops (chdir, getcwd, getdents64, mkdir) | Done |
| Environment variables | Done |
| stat / fstat / faccessat | Done |
| TIOCGWINSZ (terminal size) | Done (hardcoded 24x80) |
| isatty (fd 0/1/2) | Done |
| umask | Done |
| uname | Done |
| setpgid / setsid / getpgid | Done (stubs, flat model) |
| /etc/passwd pw_shell | Done |

### Needs Implementation

| Requirement | Used by | Effort | Phase |
|---|---|---|---|
| tcgetattr / tcsetattr | ZLE terminal mode control | Medium | 2 |
| select() or poll() | ZLE input multiplexing | Medium | 2 |
| mmap MAP_ANONYMOUS (real) | Large malloc (musl) | Medium | Shared with tcc |
| SIGWINCH delivery | Terminal resize detection | Small | 2 |
| SIGTSTP / SIGCONT | Job suspend/resume (Ctrl-Z) | Medium | 3 |
| tcsetpgrp (real) | Foreground process group | Medium | 3 |

### Not Needed (disabled at configure time)

| Feature | Configure flag |
|---|---|
| gdbm (persistent history) | --disable-gdbm |
| PCRE (Perl regex) | --disable-pcre |
| iconv (character encoding) | --without-libiconv |
| Linux capabilities | --disable-capability |
| Loadable modules (dlopen) | --disable-dynamic |
| NLS (internationalization) | --disable-nls |

## Phases

### Phase 1: Script Mode (~2-3 sessions)

Compile zsh with ZLE disabled. Works as a non-interactive scripting shell
and via `zsh -c "command"`. No new AIOS features needed -- all required
syscalls are already present.

**Configure:**
```
./configure \
    --host=aarch64-unknown-linux-gnu \
    --disable-zle \
    --disable-gdbm \
    --disable-pcre \
    --disable-dynamic \
    --disable-capability \
    --disable-nls \
    --without-libiconv \
    --without-tcsetpgrp \
    CFLAGS="-O2 -static" \
    LDFLAGS="-static"
```

**Test:**
```
zsh -c "echo hello"
zsh -c "x=hello; echo $x"
zsh -c "for i in 1 2 3; do echo num: $i; done"
zsh -c "typeset -A map; map[key]=val; echo ${map[key]}"
zsh -c "echo hello | cat | wc -c"
```

**What works in Phase 1:**
- Variable expansion, arrays, associative arrays
- Control flow (if/for/while/case/select)
- Functions (autoload, local, return)
- Pipes, redirects, command substitution
- Arithmetic (integer + floating point)
- Extended globbing
- Script files

**What does not work:**
- Interactive line editing (no ZLE)
- Tab completion
- History search
- Job control (fg, bg, Ctrl-Z)

### Phase 2: Interactive Mode (~2 sessions, requires termios)

Enable ZLE. Requires tcgetattr/tcsetattr support in the TTY subsystem
so ZLE can switch the terminal between raw and cooked modes.

**New TTY_IOCTL operations:**
```
TTY_IOCTL_TCGETS    100    Get termios struct
TTY_IOCTL_TCSETS    101    Set termios struct
TTY_IOCTL_TCSETSW   102    Set after output drain
TTY_IOCTL_TCSETSF   103    Set after output drain + input flush
```

**termios mapping to tty_server state:**

| termios field | tty_server equivalent |
|---|---|
| c_lflag & ECHO | tty_echo flag |
| c_lflag & ICANON | tty_mode (COOKED vs RAW) |
| c_lflag & ISIG | Signal generation on Ctrl-C/Z |
| c_iflag & ICRNL | CR-to-NL conversion |
| c_cc[VMIN] | Min chars for read |
| c_cc[VTIME] | Read timeout |

**Implementation in tty_server.c:**

```c
static struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[20];
} tty_termios = {
    .c_iflag = 0x0100,     /* ICRNL */
    .c_oflag = 0x0004,     /* ONLCR */
    .c_cflag = 0x00B0,     /* CS8 | CREAD */
    .c_lflag = 0x000A,     /* ECHO | ICANON */
    .c_cc = {0},
};
```

On TCSETS: update tty_termios, apply changes to tty_echo and tty_mode
based on c_lflag ECHO and ICANON bits.

On TCGETS: pack tty_termios into message registers and reply.

**posix_misc.c ioctl handler:**

Wire TCGETS (0x5401) and TCSETS (0x5402) to the new TTY_IOCTL ops
for fd 0/1/2.

**What works in Phase 2:**
- Everything from Phase 1
- Interactive line editing (vi/emacs keybindings)
- History (in-session, arrow key navigation)
- Prompt expansion
- Cursor movement, multi-line editing

### Phase 3: Job Control (longer-term)

Real job control requires the pipe_server to track process groups and
the tty_server to enforce foreground group ownership.

**Changes needed:**
- pipe_server: track pgid per process in active_proc_t
- tty_server: real tcsetpgrp (only foreground pgid reads from terminal)
- Signal delivery: SIGTSTP (Ctrl-Z) stops foreground group
- SIGCONT: resume stopped process group
- waitpid WUNTRACED: report stopped children

**What works in Phase 3:**
- Everything from Phase 2
- Ctrl-Z suspends foreground job
- `fg` / `bg` / `jobs` builtins
- `kill -STOP` / `kill -CONT`

## Cross-Compilation Strategy

zsh uses autoconf, which probes the host system. Cross-compiling requires
either:

**Option A: Configure + manual fixup**
```
./configure --host=aarch64-unknown-linux-gnu ...
```
Then fix config.h for AIOS-specific values (like dash config.h).

**Option B: Pre-generate config.h**
Create config.h manually based on AIOS capabilities (same approach
as dash). Skip configure entirely, compile source files directly
with aios-cc.

**Option B is recommended** -- it matches the dash workflow and avoids
autoconf cross-compilation issues. The config.h template:

```c
/* AIOS zsh config.h */
#define ZSH_VERSION "5.9"
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_DIRENT_H 1
#define HAVE_TERMIOS_H 1
#define HAVE_GETCWD 1
#define HAVE_WAITPID 1
#define HAVE_SIGACTION 1
#define HAVE_DUP2 1
#define HAVE_STRERROR 1
#define HAVE_MEMMOVE 1
#define HAVE_SETPGID 1

/* Disabled features */
/* #undef HAVE_GDBM_H */
/* #undef HAVE_PCRE_H */
/* #undef HAVE_ICONV */
/* #undef HAVE_DLOPEN */
/* #undef HAVE_GETRLIMIT */
/* #undef HAVE_TCSETPGRP */   /* Phase 3 */

/* Paths */
#define MODULE_DIR "/usr/lib/zsh"
#define SITEFPATH_DIR "/usr/share/zsh/site-functions"
#define FPATH_DIR "/usr/share/zsh/functions"
```

## Resource Comparison

| Resource | dash | zsh (Phase 1) | zsh (Phase 2) |
|---|---|---|---|
| Binary size | ~250KB | ~4-6MB | ~6-8MB |
| Runtime memory | ~2MB | ~8-12MB | ~12-16MB |
| Disk (headers/libs) | 0 | 0 | 0 |
| Source SLOC | 13K | 70-90K | 70-90K |
| Cross-compile time | ~10s | ~60s | ~60s |
| AIOS changes needed | 0 | 0 | termios (~200 LOC) |

## Disk Layout

```
/bin/zsh                          zsh binary (~5MB)
/usr/share/zsh/functions/         zsh autoloaded functions (optional)
/etc/zshrc                        system-wide config (optional)
/etc/passwd                       shell field: /bin/zsh for users who want it
```

Users choose their shell via /etc/passwd. Both dash and zsh coexist.

## Comparison with Alternatives

| Shell | SLOC | Interactive | Effort | Value-add over dash |
|---|---|---|---|---|
| **zsh** | 70-90K | ZLE, completion | 3-5 sessions | Arrays, extended glob, ZLE |
| mksh | ~25K | emacs mode | 2-3 sessions | Korn features, smaller |
| bash | ~176K | readline | 5-10 sessions | GNU compatibility |
| fish | ~70K (C++) | native | Not feasible | Needs C++ |

**mksh** is the simpler second shell (similar to dash effort). **zsh** is
the more capable one. Both can coexist -- the infrastructure supports
any number of shells via /etc/passwd.

## Milestones

### M1: Clone and Configure (~1 session)

- Clone zsh source
- Create AIOS config.h
- Identify source files for minimal build
- Test native compilation on host first

### M2: Cross-Compile Script Mode (~1-2 sessions)

- Cross-compile with aios-cc (or configure approach)
- Install to disk image
- Test: `zsh -c "echo hello"`, arrays, loops, functions
- Add to /etc/passwd as option

### M3: Implement termios (~1-2 sessions)

- Add TTY_IOCTL_TCGETS/TCSETS to tty_server
- Wire TCGETS/TCSETS in posix_misc.c ioctl handler
- Test: tcgetattr/tcsetattr from a test program
- Benefits zsh, and any future program needing terminal control

### M4: Interactive Mode (~1 session)

- Rebuild zsh with ZLE enabled
- Test interactive line editing
- Test history navigation
- Test prompt expansion

### M5: Job Control (future)

- Process group tracking in pipe_server
- Real tcsetpgrp in tty_server
- SIGTSTP/SIGCONT delivery
- Test: Ctrl-Z, fg, bg, jobs

## Dependencies and Ordering

zsh is independent of networking and tcc. The termios work (M3) is
shared infrastructure that benefits all terminal-aware programs.

Recommended sequencing relative to other priorities:
1. tcc M1-M2 (cross-compile tcc, no AIOS changes)
2. mmap fix (benefits tcc and zsh)
3. zsh M1-M2 (script mode, no AIOS changes)
4. termios (M3, benefits zsh + future programs)
5. zsh M4 (interactive mode)
6. Networking M1-M2 (independent track)

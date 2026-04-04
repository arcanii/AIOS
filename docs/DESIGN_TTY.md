# AIOS TTY Subsystem Design

## Executive Summary

AIOS needs a proper Unix TTY layer to support multiple users, virtual terminals, and
future network logins (SSH/telnet). This design introduces a **tty_server** protection
domain that abstracts all terminal I/O, implements line discipline, and supports both
hardware-backed virtual terminals and pseudo-terminals (PTYs) for network sessions.

The design follows the Unix model: processes read/write a TTY fd, the TTY layer handles
echo/editing/signals, and the underlying transport (serial, network, framebuffer) is
invisible to the process.

## Why: What the Current System Cannot Do

| Limitation | Root Cause | TTY Solution |
|---|---|---|
| Single user session | One serial_server, one shell | Multiple VTs, each with own login |
| No terminal abstraction | Programs talk directly to serial_ep | Programs talk to tty_ep, transport-agnostic |
| Line editing in shell | mini_shell does its own cursor/history | Line discipline in tty_server (cooked mode) |
| No raw mode for editors | All I/O is cooked by shell | IOCTL to switch raw/cooked per-TTY |
| No network terminal support | Serial is the only path | PTY pairs for sshd/telnetd |
| No Ctrl-C signal delivery | Hardcoded UART poll in root | tty_server interprets Ctrl-C, sends signal via auth |
| No session/process group | Flat process model | TTY owns session, tracks foreground group |

## Linux TTY Architecture (Reference)

```
 Process (shell, vi, cat)
    │  read()/write() on fd → /dev/ttyN or /dev/pts/N
    ▼
 TTY Layer (kernel)
    │  Line discipline: echo, canonical editing, signal generation
    │  termios settings: raw/cooked, echo on/off, special chars
    ▼
 TTY Driver (kernel)
    ├─ Console driver ──→ VGA + keyboard / Serial UART
    └─ PTY driver ──→ master fd (owned by sshd/telnetd/tmux)
```

Key Unix concepts we adopt:

- **TTY**: a terminal device with input buffer, output path, and line discipline
- **Line discipline**: processes input chars (echo, backspace, Ctrl-C) before delivering to reader
- **Cooked mode (canonical)**: line-buffered, echo on, special chars active
- **Raw mode**: character-at-a-time, no echo, no special char processing
- **Session**: a login session tied to a controlling terminal
- **Foreground process group**: the group that receives TTY signals (Ctrl-C → SIGINT)
- **PTY (pseudo-terminal)**: a master/slave pair — slave looks like a TTY to the process, master is owned by a network daemon

## AIOS TTY Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                    User Processes                              │
│              (shell, ed, cat, sbase tools)                     │
│         read(fd)/write(fd) → tty_ep via POSIX shim            │
├──────────────────────────────────────────────────────────────┤
│                                                                │
│                    tty_server (CPIO boot service)               │
│                                                                │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │                  TTY State Table                          │ │
│  │                                                           │ │
│  │  tty[0]: VT0 (serial-backed)         ← boot default      │ │
│  │    .mode      = COOKED                                    │ │
│  │    .echo      = ON                                        │ │
│  │    .input_buf = ring buffer (1024 bytes)                  │ │
│  │    .line_buf  = canonical line buffer (256 bytes)         │ │
│  │    .output    = serial_ep (PUTC)                          │ │
│  │    .session   = { uid, pid, fg_pgid }                     │ │
│  │                                                           │ │
│  │  tty[1]: VT1 (serial-backed, VT-switched)                │ │
│  │    .output    = serial_ep (PUTC) when active              │ │
│  │    .output    = /dev/null when inactive                   │ │
│  │                                                           │ │
│  │  tty[2]: VT2 (serial-backed, VT-switched)                │ │
│  │                                                           │ │
│  │  tty[3]: VT3 (serial-backed, VT-switched)                │ │
│  │                                                           │ │
│  │  pty[0]: PTY pair (network-backed)                        │ │
│  │    .master_ep = held by sshd                              │ │
│  │    .slave     = looks like a TTY to the shell             │ │
│  │                                                           │ │
│  │  pty[1]: PTY pair (network-backed)                        │ │
│  └──────────────────────────────────────────────────────────┘ │
│                                                                │
│  ┌────────────┐     ┌────────────┐     ┌────────────────────┐ │
│  │ serial_ep  │     │ pipe_ep    │     │ (future) net_ep    │ │
│  │ (UART hw)  │     │ (signals)  │     │ (TCP sockets)      │ │
│  └────────────┘     └────────────┘     └────────────────────┘ │
│                                                                │
├──────────────────────────────────────────────────────────────┤
│                    seL4 kernel                                  │
└──────────────────────────────────────────────────────────────┘
```

## TTY State Structure

```c
#define MAX_TTYS     4     /* VT0..VT3 */
#define MAX_PTYS     4     /* pts/0..pts/3 */
#define TTY_IBUF_SZ  1024  /* input ring buffer */
#define TTY_LBUF_SZ  256   /* canonical line buffer */
#define TTY_OBUF_SZ  4096  /* output buffer for inactive VTs */

typedef struct {
    int  id;                        /* tty number (0-3) or pty number */
    int  type;                      /* TTY_TYPE_VT or TTY_TYPE_PTY */
    int  active;                    /* 1 if this VT is displayed */
    int  allocated;                 /* 1 if in use */

    /* Line discipline */
    int  mode;                      /* TTY_COOKED or TTY_RAW */
    int  echo;                      /* echo input chars to output */
    int  sig_enabled;               /* interpret Ctrl-C/Z/\ */

    /* Input: ring buffer for raw chars, line buffer for cooked */
    char ibuf[TTY_IBUF_SZ];
    int  ibuf_head, ibuf_tail;
    char lbuf[TTY_LBUF_SZ];        /* canonical line being edited */
    int  lbuf_pos;                  /* cursor position in line */
    int  lbuf_len;                  /* length of line */
    int  line_ready;                /* complete line available */

    /* Output */
    char obuf[TTY_OBUF_SZ];        /* buffered output (inactive VTs) */
    int  obuf_len;

    /* Transport */
    seL4_CPtr output_ep;           /* where to send chars (serial_ep or master_ep) */

    /* Session */
    int  session_uid;
    int  session_pid;               /* login shell PID */
    int  fg_pid;                    /* foreground process PID */

    /* Blocked reader (if process waiting for input) */
    seL4_CPtr waiting_reply;        /* saved reply cap */
    int  waiting_maxlen;            /* how many bytes they want */
    int  has_waiter;                /* is someone blocked on read */
} tty_t;

typedef struct {
    tty_t  master;                  /* network daemon side */
    tty_t  slave;                   /* process side (looks like a TTY) */
} pty_pair_t;
```

## IPC Protocol

### Labels

```c
/* TTY IPC labels (70-89 range) */
#define TTY_WRITE       70   /* Write to TTY output */
#define TTY_READ        71   /* Read from TTY input (may block) */
#define TTY_IOCTL       72   /* Set/get terminal attributes */
#define TTY_OPEN        73   /* Open/attach to a TTY */
#define TTY_CLOSE       74   /* Detach from TTY */
#define TTY_INPUT       75   /* Deliver input char (from serial_server or root) */
#define TTY_SWITCH      76   /* Switch active VT (hotkey) */
#define TTY_GETATTR     77   /* Get termios-like attributes */
#define TTY_SETFG       78   /* Set foreground process */

/* PTY IPC labels */
#define PTY_ALLOC       80   /* Allocate a PTY master/slave pair */
#define PTY_WRITE_M     81   /* Write to master side (from network daemon) */
#define PTY_READ_M      82   /* Read from master side (to network daemon) */
#define PTY_FREE        83   /* Free PTY pair */
```

### Message Formats

**TTY_WRITE** (process → tty_server):
```
MR0 = tty_id
MR1 = length
MR2.. = data (packed 8 chars per MR)
Reply: MR0 = bytes_written
```

**TTY_READ** (process → tty_server):
```
MR0 = tty_id
MR1 = max_length
Reply: MR0 = bytes_read, MR1.. = data
Note: In cooked mode, blocks until line_ready. In raw mode, returns immediately
      with available chars (or blocks until at least 1 char available).
```

**TTY_IOCTL** (process → tty_server):
```
MR0 = tty_id
MR1 = operation (IOCTL_SET_RAW, IOCTL_SET_COOKED, IOCTL_ECHO_ON, IOCTL_ECHO_OFF,
                  IOCTL_GET_WINSIZE, IOCTL_SET_WINSIZE)
MR2.. = operation-specific data
Reply: MR0 = result
```

**TTY_INPUT** (root/serial_server → tty_server):
```
MR0 = char
Note: Delivered to the active VT. tty_server applies line discipline.
```

**TTY_SWITCH** (root → tty_server):
```
MR0 = target_vt (0-3)
Note: Switches active VT. Flushes inactive VT's output buffer on switch-in.
```

## Line Discipline

The line discipline is the core of the TTY layer. It processes input characters
before delivering them to the reading process.

### Cooked Mode (Canonical)

Characters are buffered until Enter (newline). The user can edit the line
with backspace and Ctrl-U (kill line). Special characters generate signals.

```
Input char
    │
    ├─ Ctrl-C (0x03) → kill foreground process (via pipe_server PIPE_KILL)
    ├─ Ctrl-Z (0x1A) → suspend foreground (future: SIGTSTP)
    ├─ Ctrl-D (0x04) → if lbuf empty: return EOF; else: flush line without newline
    ├─ Ctrl-U (0x15) → erase entire line
    ├─ Backspace/DEL → erase last char, echo backspace if echo on
    ├─ Enter/LF (0x0A, 0x0D) → line_ready=1, wake blocked reader
    │                            echo newline
    └─ Printable char → append to lbuf, echo if echo on
```

### Raw Mode

Characters are delivered immediately. No echo, no editing, no signals.
Used by: ed, vi, curses programs, password input.

```
Input char → append to ibuf ring → wake blocked reader if has_waiter
```

### Echo

When echo is ON, every input character is immediately written to the TTY output.
This includes the backspace sequence (BS + space + BS to erase on screen).

Password input: raw mode + echo OFF (or cooked + echo OFF for line-at-a-time).

## Virtual Terminal Switching

All 4 VTs share the single physical UART. Only the **active** VT sends output
to serial_server. Inactive VTs buffer output (up to TTY_OBUF_SZ).

### Hotkey Detection

Root task polls UART. When it detects the VT-switch sequence:

```
ESC [ 1 ~    → VT0 (F1 in many terminal emulators)
ESC [ 2 ~    → VT1 (F2)
ESC [ 3 ~    → VT2 (F3)
ESC [ 4 ~    → VT3 (F4)
```

Alternative (simpler, works in all terminals):
```
Ctrl-A 1     → VT0 (screen/tmux style)
Ctrl-A 2     → VT1
Ctrl-A 3     → VT2
Ctrl-A 4     → VT3
```

Root sends TTY_SWITCH IPC to tty_server. tty_server:
1. Marks old VT inactive, buffers its output
2. Marks new VT active
3. Flushes new VT's output buffer to serial_server
4. Routes all subsequent TTY_INPUT to new VT

### VT Switch Output

On switch, tty_server sends an ANSI clear-screen sequence and optionally
redraws a status bar:

```
\033[2J\033[H           ← clear screen, cursor home
[VT1] root@aios $      ← optional status indicator
```

## Boot Sequence (Revised)

```
Phase 1: Kernel boot
  1. seL4 boots, creates root task
  2. aios_root receives BootInfo, inits allocman/vka/vspace

Phase 2: Core services
  3. Map UART, init blk driver, mount ext2 + VFS
  4. Spawn serial_server (CPIO) — raw UART I/O only
  5. Spawn auth_server (CPIO) — loads /etc/passwd
  6. Spawn tty_server (CPIO) — creates VT0 backed by serial_ep

Phase 3: Login
  7. tty_server spawns getty on VT0 via exec_thread
  8. getty opens TTY, prints "AIOS login: ", authenticates via auth IPC
  9. On success, getty execs shell (or tty_server spawns shell on VT0)
  10. Shell's stdin/stdout/stderr are all VT0's tty_ep

Phase 4: Additional VTs (on-demand)
  11. User presses Ctrl-A 2 → root sends TTY_SWITCH(1) to tty_server
  12. tty_server allocates VT1 if not already allocated
  13. tty_server spawns getty on VT1
  14. User sees fresh login prompt on VT1

Phase 5: Network terminals (future, post-networking)
  15. sshd listens on TCP port 22
  16. On connection: sshd sends PTY_ALLOC to tty_server, gets master/slave
  17. sshd spawns shell attached to slave side of PTY
  18. sshd bridges TCP ↔ PTY master
```

## Process Changes

### getty (new program, in CPIO)

Minimal login program. Replaces login logic currently in mini_shell.

```c
int main(int argc, char **argv) {
    int tty_id = atoi(argv[...]);  /* which TTY to attach to */

    /* Set cooked mode + echo ON */
    tty_ioctl(tty_id, IOCTL_SET_COOKED);
    tty_ioctl(tty_id, IOCTL_ECHO_ON);

    while (1) {
        tty_write(tty_id, "AIOS login: ");
        char user[64];
        tty_read_line(tty_id, user, sizeof(user));

        tty_ioctl(tty_id, IOCTL_ECHO_OFF);
        tty_write(tty_id, "Password: ");
        char pass[64];
        tty_read_line(tty_id, pass, sizeof(pass));
        tty_ioctl(tty_id, IOCTL_ECHO_ON);
        tty_write(tty_id, "\n");

        if (auth_login(user, pass) == 0) {
            tty_write(tty_id, "Welcome, ");
            tty_write(tty_id, user);
            tty_write(tty_id, "\n");
            /* exec shell with tty_id as controlling terminal */
            exec_shell(tty_id);
            /* shell exited — loop back to login */
        } else {
            tty_write(tty_id, "Login incorrect\n\n");
        }
    }
}
```

### mini_shell changes

- Remove all login logic (getty handles this)
- Remove line editing (tty_server cooked mode handles this)
- Remove password masking (getty + echo OFF handles this)
- Keep: command parsing, pipes, exec, job control, builtins
- Add: TTY_SETFG IPC before exec (set foreground process)
- Add: TTY_IOCTL for programs that need raw mode

### aios_posix.c changes

The POSIX shim needs to route read/write through tty_server:

```c
/* Current: write(1, buf, len) → ser_putc() each char */
/* New:     write(1, buf, len) → TTY_WRITE IPC to tty_server */

/* Current: read(0, buf, len)  → ser_getc() looping */
/* New:     read(0, buf, len)  → TTY_READ IPC to tty_server (blocks in cooked) */
```

File descriptors 0, 1, 2 are mapped to the process's controlling TTY.
The tty_ep (badged per-TTY) is passed via argv like serial_ep is today.

### Capability Layout

Each process currently receives via argv:
```
[serial_ep, fs_ep, thread_ep, auth_ep, pipe_ep, CWD, progname, args...]
```

New layout adds tty_ep, removes serial_ep from direct process access:
```
[tty_ep, fs_ep, thread_ep, auth_ep, pipe_ep, CWD, progname, args...]
```

The tty_ep is badged with the TTY ID so tty_server knows which terminal
the process belongs to. serial_ep is only held by tty_server itself.

## POSIX Mapping

| POSIX call | AIOS implementation |
|---|---|
| read(0, ...) | TTY_READ IPC, blocks in cooked until line ready |
| write(1, ...) | TTY_WRITE IPC |
| tcgetattr() / tcsetattr() | TTY_GETATTR / TTY_IOCTL |
| tcsetpgrp() | TTY_SETFG |
| isatty() | returns 1 if fd maps to tty_ep |
| ttyname() | returns "/dev/ttyN" based on badge |
| ioctl(TIOCGWINSZ) | TTY_IOCTL with IOCTL_GET_WINSIZE |
| openpty() | PTY_ALLOC IPC, returns master_fd + slave_fd |

## Phased Implementation

### Phase 1: tty_server with single VT0 (replaces serial_server for I/O)

**Goal**: All process I/O goes through tty_server. serial_server becomes a dumb UART driver.

Files changed:
- `src/apps/tty_server.c` (new) — TTY state machine, line discipline, IPC loop
- `src/aios_root.c` — spawn tty_server, pass serial_ep to it, route KEY_PUSH to tty_server
- `src/lib/aios_posix.c` — read/write fd 0/1/2 via tty_ep instead of serial_ep
- `src/lib/aios_posix.h` — add TTY IPC labels
- `include/aios/tty.h` (new) — TTY constants, ioctl definitions
- `projects/aios/CMakeLists.txt` — add tty_server to CPIO boot apps

**Verification**: Boot, login, `ls`, `cat /etc/motd` all work through tty_server.
Line editing (backspace, Ctrl-U) works in cooked mode.

### Phase 2: getty + shell separation

**Goal**: Login handled by getty, shell is just a shell.

Files changed:
- `src/apps/getty.c` (new) — login loop, execs shell
- `src/apps/mini_shell.c` — remove login/password code, assume authenticated session
- `src/aios_root.c` — spawn getty instead of mini_shell at boot

**Verification**: Boot → getty shows login prompt → authenticate → shell.
Exit shell → getty shows login prompt again.

### Phase 3: Multiple virtual terminals

**Goal**: Ctrl-A 1/2/3/4 switches between 4 VTs. Each has independent login.

Files changed:
- `src/apps/tty_server.c` — VT switching, per-VT output buffering
- `src/aios_root.c` — hotkey detection in UART poll, TTY_SWITCH IPC

**Verification**: Ctrl-A 2 opens VT1 with fresh login. Ctrl-A 1 returns to VT0.
Output on inactive VTs is buffered and displayed on switch.

### Phase 4: Raw mode + ioctl

**Goal**: Programs like ed can use raw mode for character-at-a-time input.

Files changed:
- `src/apps/tty_server.c` — TTY_IOCTL handler, raw mode path
- `src/lib/aios_posix.c` — tcgetattr/tcsetattr/ioctl wrappers

**Verification**: `ed` works without spurious syscall errors.
Password entry uses echo-off correctly.

### Phase 5: PTY support (post-networking)

**Goal**: sshd can create PTY pairs for remote sessions.

Files changed:
- `src/apps/tty_server.c` — PTY_ALLOC, master/slave routing
- Future sshd program

**Verification**: PTY master/slave pair created. Data flows bidirectionally.

## Design Decisions

| Decision | Rationale |
|---|---|
| tty_server as separate CPIO PD | Isolates TTY state from root; can crash without killing system |
| Badge per-TTY on tty_ep | Server knows which VT without extra MR; same pattern as fs_ep badge |
| Cooked mode as default | Most programs expect line-buffered input with echo |
| Ctrl-A prefix for VT switch | Works in all terminal emulators; avoids conflicts with Ctrl-F1..F4 which QEMU may intercept |
| getty as separate program | Unix convention; clean separation of login from shell |
| tty_ep replaces serial_ep in argv | Programs don't need to know about serial; tty is the abstraction |
| Output buffering for inactive VTs | Prevents lost output; can review when switching back |
| Line discipline in tty_server not kernel | seL4 is a microkernel; policy belongs in userspace |

## Risk Assessment

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| IPC overhead vs direct serial | Low | Slightly slower output | seL4 IPC is ~100 cycles; barely noticeable for terminal I/O |
| Blocked reader complexity | Medium | Deadlock if reply cap mishandled | Use SaveCaller pattern from LEARNINGS.md |
| VT switch during pipe | Medium | Output goes to wrong VT | Badge identifies TTY; pipe output follows process's TTY not active VT |
| Raw mode breaks existing programs | Low | Programs see unexpected chars | Default is cooked; only explicit ioctl switches to raw |
| Buffer overflow on inactive VTs | Low | Lost output | Cap buffer at 4KB, discard oldest when full |

## Testing Plan

1. **Basic I/O**: echo, cat, ls work through tty_server
2. **Line discipline**: backspace erases, Ctrl-U kills line, Ctrl-D sends EOF
3. **Cooked mode**: line-buffered reads work (cat waits for Enter)
4. **Signal generation**: Ctrl-C kills foreground process
5. **VT switching**: Ctrl-A 1/2/3/4 switches, output preserved
6. **Multi-user**: Two users logged in on VT0 and VT1 simultaneously
7. **Raw mode**: ed/password entry works
8. **Stress**: Rapid VT switching during output doesn't corrupt buffers

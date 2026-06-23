# Design: PTY-backed SSH sessions (a comfortable daily-driver console)

- **Date:** 2026-06-24 (session 14)
- **Goal:** make `sshd` a real terminal so interactive shells (zsh ZLE), `vi`, `less`, line
  editing, history, tab-completion, signals, and resize all work over SSH.
- **Root problem (confirmed in code):** `ssh_channel.c:spawn_shell` wires the shell's stdin/stdout/
  stderr to plain **pipes**, and the sshd relays a "cooked" byte stream. There is no PTY, so
  `isatty()` is false and raw-mode editors wedge -- the code says so verbatim: *"zsh wedged its
  raw-mode ZLE over the cooked SSH relay"* (`ssh_channel.c:506`), which is why a SIGKILL self-heal
  exists. Meanwhile `tty_server` (`src/apps/tty_server.c`, 504 lines) ALREADY has the full line
  discipline (raw + cooked, ISIG/ICANON/ECHO, Ctrl-C/U, termios, VMIN/VTIME) -- but as ONE global
  instance bound to the serial/keyboard console. The SSH path just bypasses it.

## Approach: multi-instance `tty_server` (one PTY per session)
Reuse the proven line discipline. Make `tty_server` hold N TTY instances instead of one:
- **instance 0** = the serial/keyboard console (output -> mini-UART + HDMI fb_console). UNCHANGED.
- **instances 1..N-1** = PTYs. A PTY's "slave" side is the shell (read/write via the TTY protocol);
  its "master" side is the sshd (feeds client keystrokes in, reads shell output + echo out).

### State refactor (`tty_server.c`)
Bundle today's globals into a struct, array it:
```c
typedef struct {
    int      used;
    char     key_buf[KEY_BUF_SZ];  int key_head, key_tail;      /* raw input ring */
    char     line_buf[LINE_BUF_SZ]; int line_len;               /* cooked line being built */
    char     line_queue[LINE_QUEUE_SZ]; int lq_head, lq_tail;   /* completed lines for TTY_READ */
    uint32_t iflag, oflag, cflag, lflag; uint8_t cc[20];        /* termios */
    int      mode, echo, isig, icrnl;                           /* derived */
    uint16_t ws_row, ws_col;                                    /* winsize */
    /* output target: instance 0 -> serial+HDMI; PTY -> master_out ring below */
    int      is_pty;
    char     master_out[MASTER_OUT_SZ]; int mo_head, mo_tail;   /* PTY: shell output + echo for sshd */
} tty_inst_t;
static tty_inst_t tty_inst[MAX_TTY];   /* MAX_TTY ~= 8 */
```
`line_discipline()`, the read/write/ioctl handlers, and `termios_sync()` all take an instance index.
The ONLY behavioural fork: the output helper. Instance 0 -> `seL4_DebugPutChar` + `disp_write`
(today). A PTY -> push to `master_out` (the sshd drains it).

### IPC additions (`root_shared.h`)
Keep the existing TTY_WRITE(70)/TTY_READ(71)/TTY_IOCTL(72)/TTY_INPUT(75)/TTY_POLL(76); add an
instance id (MR1) to each so the slave-side shell addresses its own PTY. New master-side ops:
- `TTY_PTY_ALLOC`  -> returns a free instance id (or <0). sshd calls on `pty-req`.
- `TTY_PTY_MASTER_READ id` -> drain `master_out` (shell output + echo) -> sshd sends to SSH client.
- `TTY_PTY_WINSZ id rows cols` -> set winsize + raise SIGWINCH to the fg proc (window-change).
- `TTY_PTY_FREE id` -> release on channel close.
(`TTY_INPUT id` already exists for feeding keystrokes; sshd uses it for client->shell.)

### fd routing (the other half)
A shell on a PTY needs fd 0/1/2 to be `is_tty` AND carry the PTY instance id, so `read`/`write`/
`ioctl` route to the right instance. Today `is_tty` is a bool ([[feedback_is_tty_routing]]); extend
`aios_fd_t` with a `tty_inst` field (0 = serial). `spawn_shell` (sshd) sets the child's 0/1/2 to the
allocated PTY instance instead of pipe fds. `posix_file.c`/`posix_misc.c` pass `tty_inst` in the
TTY_* MRs.

### sshd relay rewrite (`ssh_channel.c`)
On `pty-req`: `TTY_PTY_ALLOC` -> instance id; apply requested modes (raw when the client wants a PTY)
+ winsize. `spawn_shell`: child fd 0/1/2 -> the PTY instance (is_tty + id), not pipes. Relay loop:
SSH channel data -> `TTY_INPUT id` (keystrokes into the line discipline); `TTY_PTY_MASTER_READ id` ->
SSH channel (shell output + echo). `window-change` -> `TTY_PTY_WINSZ`. Drop the hand-rolled Ctrl-C
interception -- the line discipline's ISIG now generates SIGINT correctly. The SIGKILL self-heal can
stay as a backstop but should stop firing (no more ZLE wedge).

## Incremental plan (each step builds + QEMU-gates; serial console must never regress)
1. **Multi-instance scaffolding** -- refactor `tty_server` globals into `tty_inst[]`; instance 0
   behaves EXACTLY as today (output->serial+HDMI). No PTY, no protocol change yet. Gate: boot +
   serial console + smp 4/5 unchanged.
2. **PTY protocol** -- add the instance-id arg to TTY_* + the new TTY_PTY_* ops + the `master_out`
   path. Add `tty_inst` to `aios_fd_t` + route in posix_file/posix_misc. Still no sshd change. Gate:
   serial console unchanged; a unit-style probe allocates a PTY and round-trips bytes.
3. **sshd uses the PTY** -- rewire `spawn_shell` + the relay; handle `window-change`. Gate: SSH in,
   run `dash` interactively; isatty true; Ctrl-C works; `vi`/`less` render. THE payoff step.
4. **zsh + polish** -- point `SSH_SHELL_PATH` at interactive zsh (Tier-2 dependency: zsh ZLE + select/
   poll); verify resize/SIGWINCH; confirm concurrent sessions (the relay calls sshd "one-connection-
   at-a-time" -- verify/loosen).

## Risks
- **Serial console regression** -- the refactor touches the one working console. Mitigate: step 1 keeps
  instance 0 byte-identical; gate every step on a serial-console smoke test.
- **fd inheritance across fork+exec** -- the child's `tty_inst` must survive `PIPE_EXEC` (pipe_server
  preserves slot state across exec; verify it carries `tty_inst`).
- **Blocking reads** -- the slave shell's `read(0)` must block until input or signal; the master
  `TTY_PTY_MASTER_READ` must not busy-spin (use the existing poll/park pattern).
- **Concurrency** -- multiple PTYs imply multiple blocked readers; ensure tty_server's single-threaded
  loop services all instances (it already deferred-replies; per-instance reply caps).
- **zsh dependency** -- step 4 needs interactive zsh working (Tier-2). Steps 1-3 deliver a real PTY for
  `dash`/`vi`/`less` independent of zsh; zsh is the cherry on top.

## Payoff
Steps 1-3 alone turn SSH from a cooked-pipe relay into a real terminal: line editing, signals,
`isatty`, full-screen apps, resize. This is THE step that makes AIOS feel like a real OS to use, and
it is the shared prerequisite for interactive zsh (Tier-2). Reuses the existing, proven line
discipline rather than adding a second one.

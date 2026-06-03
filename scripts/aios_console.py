#!/usr/bin/env python3
"""aios_console.py -- drive a running AIOS over a QEMU socket or serial.

Pure Python stdlib. No pty is allocated, so this works in environments
where screen / expect fail with "no more ptys" (e.g. headless agents,
sandboxes). Two transports:

  qemu    Launch qemu-system-aarch64 with the console on a unix socket
          (-serial unix:...,server) and drive it. Fully self-contained:
          boot -> login -> run commands -> optional shutdown -> teardown.

  serial  Open a serial char device (e.g. /dev/cu.usbserial-0001) and
          drive whatever is on the other end (a real RPi4). Configures
          8N1 at the given baud via termios. Only one process may own the
          port -- quit screen first.

  watch   Live-render a session mirror file on screen (read-only, colored),
          so a human can watch the input/output while another process drives
          the console. qemu/serial mirror their transcript to DEFAULT_MIRROR
          by default; run `watch` in a second terminal to follow it live.
          (Only one process can own the port, so the viewer taps the mirror
          file rather than the device.)

Examples:
  scripts/aios_console.py qemu \\
      --kernel build-04/images/aios_root-image-arm-qemu-arm-virt --smp 4 \\
      --cmd 'echo abc | wc -c' --cmd 'cat /proc/hw' --shutdown

  scripts/aios_console.py serial /dev/cu.usbserial-0001 \\
      --cmd 'cat /proc/hw' --cmd 'echo abc | wc -c'

  # Watch live while another process drives:
  #   terminal 1 (you):    scripts/aios_console.py watch
  #   terminal 2 (driver): scripts/aios_console.py serial /dev/cu... --login --cmd '...'

Notes:
  * Commands are matched against the dash prompt "# ". Output that itself
    contains "# " could confuse the match; the diagnostic commands we use
    do not. Boot/fault noise interleaves freely and is captured verbatim.
  * Everything received is streamed to stdout (and --log FILE if given),
    so the full transcript -- including the BSS/COW fault spew -- is
    visible. Run commands one per --cmd; they are sent sequentially.
  * Paths and the default disks are resolved from the repo root, so run
    it from anywhere.
"""

import argparse
import os
import select
import socket
import subprocess
import sys
import termios
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LOGIN_PROMPT = "AIOS login:"
PASSWORD_PROMPT = "Password:"
SHELL_PROMPT = "# "
WELCOME = "Welcome,"

# Real-time session transcript that the `watch` subcommand renders live, so a
# human can watch the I/O on screen while something else drives the console
# (only one process may own the port, so the viewer taps this file, not the fd).
# Kept under the repo root so the driver and the viewer -- even in different
# shells/sandboxes -- resolve the SAME path (a fixed /tmp can differ between
# them). Gitignored.
DEFAULT_MIRROR = os.path.join(REPO_ROOT, ".aios_console.live")


class Console:
    """Expect-style interaction over a single fd (socket or serial)."""

    def __init__(self, fd, logfile=None, echo=True, mirrorfile=None):
        self.fd = fd
        self.buf = ""
        self.logfile = logfile
        self.echo = echo
        self.mirrorfile = mirrorfile

    def _emit(self, text):
        if self.echo:
            sys.stdout.write(text)
            sys.stdout.flush()
        if self.logfile:
            self.logfile.write(text)
            self.logfile.flush()
        if self.mirrorfile:
            self.mirrorfile.write(text)
            self.mirrorfile.flush()

    def mark_tx(self, label):
        """Annotate the transcript with input we are sending, so the live
        viewer shows both directions. RX echoes commands back, but login
        keystrokes and nudges are otherwise invisible."""
        self._emit("\n>>> %s\n" % label)

    def read_until(self, patterns, timeout):
        """Read until any pattern (substring) appears or timeout elapses.

        Returns (matched_pattern_or_None, text_consumed). On a match the
        rolling buffer is consumed up to and including the pattern.
        """
        if isinstance(patterns, str):
            patterns = [patterns]
        deadline = time.time() + timeout
        while True:
            best = -1
            best_pat = None
            for p in patterns:
                idx = self.buf.find(p)
                if idx != -1 and (best == -1 or idx < best):
                    best = idx
                    best_pat = p
            if best != -1:
                consumed = self.buf[:best + len(best_pat)]
                self.buf = self.buf[best + len(best_pat):]
                return best_pat, consumed
            remaining = deadline - time.time()
            if remaining <= 0:
                return None, ""
            r, _, _ = select.select([self.fd], [], [], min(remaining, 1.0))
            if not r:
                continue
            try:
                data = os.read(self.fd, 4096)
            except OSError:
                return None, ""
            if not data:
                time.sleep(0.05)
                continue
            text = data.decode("utf-8", "replace")
            self._emit(text)
            self.buf += text
            if len(self.buf) > 1 << 20:
                self.buf = self.buf[-(1 << 19):]

    def send(self, s):
        data = s.encode("utf-8")
        while data:
            n = os.write(self.fd, data)
            data = data[n:]

    def sendline(self, s=""):
        self.send(s + "\r")

    def ensure_shell(self, user, password, timeout, nudge=False, settle=0.5):
        """Reach a shell prompt: log in if needed, else detect existing."""
        if nudge:
            self.sendline("")
        pat, _ = self.read_until([LOGIN_PROMPT, SHELL_PROMPT], timeout)
        if pat == SHELL_PROMPT:
            return
        if pat is None:
            raise TimeoutError("no login or shell prompt within %ss" % timeout)
        # getty needs a moment to switch into (no-echo) password-read mode.
        # Sending the credentials the instant the prompt appears races that
        # switch and corrupts the line (first char echoes unmasked -> the
        # field is mis-read -> login denied), so settle briefly each time.
        time.sleep(settle)
        self.mark_tx("login: %s" % user)
        self.sendline(user)
        if self.read_until(PASSWORD_PROMPT, 30)[0] is None:
            raise TimeoutError("no password prompt")
        time.sleep(settle)
        self.mark_tx("password: ****")
        self.sendline(password)
        pat, _ = self.read_until([SHELL_PROMPT, WELCOME, LOGIN_PROMPT], 30)
        if pat == LOGIN_PROMPT:
            raise RuntimeError("login denied -- check --user/--password")
        if pat is None:
            raise TimeoutError("login did not reach a shell")
        if pat == WELCOME:
            self.read_until(SHELL_PROMPT, 30)

    def run(self, cmd, timeout):
        """Send one command, capture through the next prompt."""
        self._emit("\n===CMD: %s===\n" % cmd)
        self.sendline(cmd)
        pat, out = self.read_until(SHELL_PROMPT, timeout)
        if pat is None:
            self._emit("\n===TIMEOUT after %ss waiting for prompt===\n" % timeout)
        return out


def configure_serial(fd, baud):
    """Set raw 8N1 at baud on a serial fd via termios."""
    speed = getattr(termios, "B%d" % baud, None)
    if speed is None:
        raise ValueError("unsupported baud: %d" % baud)
    a = termios.tcgetattr(fd)
    # a = [iflag, oflag, cflag, lflag, ispeed, ospeed, cc]
    a[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP
              | termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
    a[1] &= ~termios.OPOST
    a[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)
    a[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    a[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG
              | termios.IEXTEN)
    a[4] = speed
    a[5] = speed
    termios.tcsetattr(fd, termios.TCSANOW, a)


def qemu_command(args, sock_path):
    cmd = [
        "qemu-system-aarch64",
        "-machine", "virt,virtualization=on",
        "-cpu", "cortex-a53",
        "-smp", str(args.smp),
        "-m", args.mem,
        "-display", "none",
        "-monitor", "none",
        "-no-reboot",
        "-serial", "unix:%s,server" % sock_path,
        "-kernel", args.kernel,
    ]
    drives = args.disk
    if drives is None:
        drives = [p for p in ("disk/disk_ext2.img", "disk/log_ext2.img")
                  if os.path.exists(p)]
    for i, path in enumerate(drives):
        cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (path, i),
                "-device", "virtio-blk-device,drive=hd%d" % i]
    return cmd


def connect_qemu_socket(path, timeout=15.0):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            return s
        except OSError as e:
            last = e
            time.sleep(0.1)
    raise TimeoutError("could not connect to qemu socket %s (%s)" % (path, last))


def open_mirror(args):
    """Open the real-time transcript mirror (truncating) unless disabled.

    The `watch` subcommand renders this file live, letting a human watch the
    session on screen while this process drives it headlessly."""
    if getattr(args, "no_mirror", False):
        return None
    path = getattr(args, "mirror", None) or DEFAULT_MIRROR
    try:
        f = open(path, "w")
    except OSError as e:
        print("WARN: live mirror disabled (%s)" % e, file=sys.stderr)
        return None
    f.write("=== aios_console %s @ pid %d -- live transcript ===\n"
            % (args.mode, os.getpid()))
    f.flush()
    print("=== live mirror -> %s  (watch with: python3 scripts/aios_console.py "
          "watch) ===" % path, flush=True)
    return f


def run_qemu(args):
    sock_path = os.path.join("/tmp", "aios-console-%d.sock" % os.getpid())
    if os.path.exists(sock_path):
        os.unlink(sock_path)
    cmd = qemu_command(args, sock_path)
    print("=== launching: %s ===" % " ".join(cmd), flush=True)
    try:
        proc = subprocess.Popen(cmd)
    except FileNotFoundError:
        print("FAIL: qemu-system-aarch64 not found on PATH", file=sys.stderr)
        return 2
    sock = None
    logf = open(args.log, "w") if args.log else None
    mirf = open_mirror(args)
    rc = 0
    try:
        sock = connect_qemu_socket(sock_path)
        con = Console(sock.fileno(), logfile=logf, echo=not args.quiet,
                      mirrorfile=mirf)
        con.ensure_shell(args.user, args.password, args.boot_timeout, nudge=False)
        for c in args.cmd or []:
            con.run(c, args.cmd_timeout)
        if args.shutdown:
            con._emit("\n===CMD: shutdown===\n")
            con.sendline("shutdown")
            con.read_until("__never__never__", min(args.cmd_timeout, 15))
    except (TimeoutError, RuntimeError, OSError) as e:
        print("\nFAIL: %s" % e, file=sys.stderr)
        rc = 1
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        if logf:
            logf.close()
        if mirf:
            mirf.close()
        if os.path.exists(sock_path):
            try:
                os.unlink(sock_path)
            except OSError:
                pass
    print("\n=== qemu console done (exit %d) ===" % rc, flush=True)
    return rc


def run_serial(args):
    logf = open(args.log, "w") if args.log else None
    mirf = open_mirror(args)
    fd = None
    rc = 0
    try:
        fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY)
        configure_serial(fd, args.baud)
        con = Console(fd, logfile=logf, echo=not args.quiet, mirrorfile=mirf)
        if args.login:
            con.ensure_shell(args.user, args.password, args.boot_timeout,
                             nudge=True)
        for c in args.cmd or []:
            con.run(c, args.cmd_timeout)
        if args.shutdown:
            con._emit("\n===CMD: shutdown===\n")
            con.sendline("shutdown")
            con.read_until("__never__never__", min(args.cmd_timeout, 15))
    except (TimeoutError, RuntimeError, OSError, ValueError) as e:
        print("\nFAIL: %s" % e, file=sys.stderr)
        rc = 1
    finally:
        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass
        if logf:
            logf.close()
        if mirf:
            mirf.close()
    print("\n=== serial console done (exit %d) ===" % rc, flush=True)
    return rc


# ── live viewer ──────────────────────────────────────────────────────────────
_ANSI = {
    "reset": "\033[0m", "cmd": "\033[1;36m", "err": "\033[1;31m",
    "wrn": "\033[33m", "ok": "\033[1;32m", "tx": "\033[1;35m",
    "dim": "\033[2m", "num": "\033[1;37m",
}


def _colorize(line):
    """Color a transcript line by content for the live viewer."""
    s = line.rstrip("\r\n")
    if "===CMD:" in s:
        c = _ANSI["cmd"]
    elif s.startswith(">>> "):
        c = _ANSI["tx"]
    elif ("TIMEOUT" in s or "FAIL:" in s or "[ERR]" in s or "denied" in s
          or "not permitted" in s or "not reserved" in s or "map failed" in s):
        c = _ANSI["err"]
    elif "[WRN]" in s:
        c = _ANSI["wrn"]
    elif ("console done" in s or "login OK" in s or "Welcome" in s
          or "shutdown complete" in s):
        c = _ANSI["ok"]
    elif s.strip().isdigit():            # a bare wc/seq count -- the result
        c = _ANSI["num"]
    elif s.startswith("[INF]") or "cow_setup" in s or "BSS lazy" in s:
        c = _ANSI["dim"]
    else:
        c = ""
    return (c + line + _ANSI["reset"]) if c else line


def run_watch(args):
    """Live-render a session mirror file (read-only) with color, so a human
    can watch the I/O on screen while another process drives the console.
    Tails the file, follows truncation/recreation (new session), and flushes
    lingering partial lines (e.g. the bare '# ' prompt)."""
    path = args.file or DEFAULT_MIRROR
    color = sys.stdout.isatty() and not args.no_color

    def emit(text):
        sys.stdout.write(_colorize(text) if color else text)

    ok = _ANSI["ok"] if color else ""
    rst = _ANSI["reset"] if color else ""
    if color:
        sys.stdout.write("\033[2J\033[H")
    sys.stdout.write("%s== aios_console watch: %s ==%s  (Ctrl-C to stop)\n\n"
                     % (ok, path, rst))
    sys.stdout.flush()

    pos, ino, pending = 0, None, ""
    try:
        while True:
            try:
                st = os.stat(path)
            except FileNotFoundError:
                time.sleep(0.2)
                continue
            if ino is None:
                ino = st.st_ino
            if st.st_ino != ino or st.st_size < pos:   # driver started fresh
                pos, pending, ino = 0, "", st.st_ino
                sys.stdout.write("\n%s-- new session --%s\n" % (ok, rst))
            if st.st_size > pos:
                with open(path, "r", errors="replace") as f:
                    f.seek(pos)
                    pending += f.read()
                    pos = f.tell()
                while True:
                    nl = pending.find("\n")
                    if nl == -1:
                        break
                    emit(pending[:nl + 1])
                    pending = pending[nl + 1:]
                sys.stdout.flush()
            else:
                if pending:               # show the trailing prompt, no newline
                    emit(pending)
                    pending = ""
                    sys.stdout.flush()
                time.sleep(0.15)
    except KeyboardInterrupt:
        sys.stdout.write("\n%s== watch stopped ==%s\n" % (ok, rst))
    return 0


def build_parser():
    p = argparse.ArgumentParser(description="Drive AIOS over qemu socket or serial.")
    sub = p.add_subparsers(dest="mode", required=True)

    common_login = dict()

    q = sub.add_parser("qemu", help="launch and drive qemu")
    q.add_argument("--kernel", required=True, help="path to aios_root image")
    q.add_argument("--smp", type=int, default=1, help="qemu cpu count (default 1)")
    q.add_argument("--mem", default="2G", help="qemu memory (default 2G)")
    q.add_argument("--disk", action="append",
                   help="raw disk image (repeatable; default disk/disk_ext2.img + disk/log_ext2.img)")

    s = sub.add_parser("serial", help="drive a serial device")
    s.add_argument("device", help="serial char device, e.g. /dev/cu.usbserial-0001")
    s.add_argument("--baud", type=int, default=115200)
    s.add_argument("--login", action="store_true",
                   help="perform login (default: assume already at a shell)")

    w = sub.add_parser("watch",
                       help="live-view a session mirror on screen (read-only)")
    w.add_argument("file", nargs="?", default=None,
                   help="mirror file to watch (default: %s)" % DEFAULT_MIRROR)
    w.add_argument("--no-color", action="store_true", help="disable ANSI color")

    for sp in (q, s):
        sp.add_argument("--cmd", action="append", help="command to run (repeatable)")
        sp.add_argument("--shutdown", action="store_true",
                        help="send 'shutdown' after the commands")
        sp.add_argument("--user", default="root")
        sp.add_argument("--password", default="root")
        sp.add_argument("--boot-timeout", type=float, default=120.0,
                        dest="boot_timeout")
        sp.add_argument("--cmd-timeout", type=float, default=30.0,
                        dest="cmd_timeout")
        sp.add_argument("--log", help="also write the transcript to this file")
        sp.add_argument("--quiet", action="store_true",
                        help="do not echo the transcript to stdout")
        sp.add_argument("--mirror", default=None,
                        help="real-time transcript file the `watch` viewer "
                             "renders live (default: %s)" % DEFAULT_MIRROR)
        sp.add_argument("--no-mirror", action="store_true",
                        help="do not write the live mirror file")
    return p


def main(argv):
    args = build_parser().parse_args(argv)
    os.chdir(REPO_ROOT)
    if args.mode == "qemu":
        return run_qemu(args)
    if args.mode == "serial":
        return run_serial(args)
    if args.mode == "watch":
        return run_watch(args)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

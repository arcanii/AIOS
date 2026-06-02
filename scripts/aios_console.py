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

Examples:
  scripts/aios_console.py qemu \\
      --kernel build-04/images/aios_root-image-arm-qemu-arm-virt --smp 4 \\
      --cmd 'echo abc | wc -c' --cmd 'cat /proc/hw' --shutdown

  scripts/aios_console.py serial /dev/cu.usbserial-0001 \\
      --cmd 'cat /proc/hw' --cmd 'echo abc | wc -c'

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


class Console:
    """Expect-style interaction over a single fd (socket or serial)."""

    def __init__(self, fd, logfile=None, echo=True):
        self.fd = fd
        self.buf = ""
        self.logfile = logfile
        self.echo = echo

    def _emit(self, text):
        if self.echo:
            sys.stdout.write(text)
            sys.stdout.flush()
        if self.logfile:
            self.logfile.write(text)
            self.logfile.flush()

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
        self.sendline(user)
        if self.read_until(PASSWORD_PROMPT, 30)[0] is None:
            raise TimeoutError("no password prompt")
        time.sleep(settle)
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
    rc = 0
    try:
        sock = connect_qemu_socket(sock_path)
        con = Console(sock.fileno(), logfile=logf, echo=not args.quiet)
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
        if os.path.exists(sock_path):
            try:
                os.unlink(sock_path)
            except OSError:
                pass
    print("\n=== qemu console done (exit %d) ===" % rc, flush=True)
    return rc


def run_serial(args):
    logf = open(args.log, "w") if args.log else None
    fd = None
    rc = 0
    try:
        fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY)
        configure_serial(fd, args.baud)
        con = Console(fd, logfile=logf, echo=not args.quiet)
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
    print("\n=== serial console done (exit %d) ===" % rc, flush=True)
    return rc


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
    return p


def main(argv):
    args = build_parser().parse_args(argv)
    os.chdir(REPO_ROOT)
    if args.mode == "qemu":
        return run_qemu(args)
    if args.mode == "serial":
        return run_serial(args)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

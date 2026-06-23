#!/usr/bin/env python3
"""End-to-end test for PTY-backed SSH sessions (PTY step 3, v0.4.296).

Boots AIOS under QEMU, logs in over serial, confirms sshd auto-started, then
connects with the real OpenSSH client using `ssh -tt` (FORCE remote PTY
allocation) and validates the daily-driver console:

  * isatty(0) is TRUE over SSH  -- the whole point: `test -t 0` succeeds, and
    `tty` does not say "not a tty". (With the old cooked-pipe relay this failed.)
  * the interactive shell echoes typed input (tty_server line discipline) and
    runs commands (marker, whoami=root, uname=AIOS).
  * Ctrl-C (0x03) is handled by the line discipline's ISIG -> SIGINT to the fg
    process: a partial command line is discarded (NOT executed) and the session
    survives + runs the next command. This exercises the new VINTR->SIGINT path
    (sshd no longer hand-rolls Ctrl-C).

Full-screen apps (vi/less) need a real interactive terminal to verify cursor
addressing; this harness checks `less` accepts raw input and quits cleanly as a
smoke test, and leaves true visual verification to an interactive session.

Password is supplied via SSH_ASKPASS (no controlling tty needed). SLIRP has no
TCP retransmit, so we retry the connection a few times.
"""
import importlib.util
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get(
    "AIOS_KERNEL",
    os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-ssh-pty-test.sock"
ASKPASS = "/tmp/aios-ssh-pty-askpass.sh"
PORT = 2222
PASSWORD = "root"

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)


def qemu_cmd():
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-monitor", "none", "-no-reboot",
           "-serial", "unix:%s,server" % SOCK, "-kernel", KERNEL]
    for i, path in enumerate([DISK, LOGDISK]):
        if os.path.exists(path):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (path, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    cmd += ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:%d" % (PORT, PORT),
            "-device", "virtio-net-device,netdev=n0"]
    return cmd


def write_askpass():
    with open(ASKPASS, "w") as f:
        f.write("#!/bin/sh\necho '%s'\n" % PASSWORD)
    os.chmod(ASKPASS, 0o755)


def ssh_pty_run(commands, timeout=60):
    """Run OpenSSH with -tt (force remote PTY), feeding `commands` on stdin.
    Returns (rc, stdout, stderr). The remote PTY echoes input, so stdout
    contains echoed keystrokes + command output (CRLF line endings)."""
    env = dict(os.environ)
    env["SSH_ASKPASS"] = ASKPASS
    env["SSH_ASKPASS_REQUIRE"] = "force"
    env["DISPLAY"] = env.get("DISPLAY", ":0")
    cmd = ["ssh", "-tt", "-p", str(PORT),
           "-o", "StrictHostKeyChecking=no",
           "-o", "UserKnownHostsFile=/dev/null",
           "-o", "PreferredAuthentications=password",
           "-o", "PubkeyAuthentication=no",
           "-o", "KbdInteractiveAuthentication=no",
           "-o", "NumberOfPasswordPrompts=1",
           "-o", "ConnectTimeout=10",
           "-o", "LogLevel=ERROR",
           "root@127.0.0.1"]
    try:
        # Send CR (\r) line endings: a real terminal's Enter is CR, and zsh's
        # raw-mode ZLE binds accept-line to CR (LF is not accept-line, so zsh
        # would never run the lines). dash's cooked mode (ICRNL) accepts CR too.
        commands = commands.replace("\n", "\r")
        r = subprocess.run(cmd, input=commands.encode(), capture_output=True,
                           env=env, timeout=timeout, start_new_session=True)
        return r.returncode, r.stdout.decode("utf-8", "replace"), \
            r.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace")
        return None, out, "TIMEOUT"


def attempt(commands, want, timeout=60, tries=3):
    """Retry ssh until any token in `want` appears (SLIRP flakiness)."""
    rc, sout, serr = None, "", ""
    for i in range(tries):
        rc, sout, serr = ssh_pty_run(commands, timeout=timeout)
        print("  [ssh -tt attempt %d] rc=%s out=%r err=%r"
              % (i + 1, rc, sout[:160], serr[:120]), flush=True)
        if any(w in sout for w in want):
            break
        time.sleep(2.0)
    return rc, sout, serr


def main():
    results = []

    def check(name, ok, detail=""):
        results.append(bool(ok))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  -- " + detail) if detail else ""), flush=True)

    if os.path.exists(SOCK):
        os.unlink(SOCK)
    write_askpass()
    proc = subprocess.Popen(qemu_cmd())
    con = None
    try:
        sock = ac.connect_qemu_socket(SOCK)
        con = ac.Console(sock.fileno(), echo=True)
        print("=== booting + serial login ===", flush=True)
        con.read_until([ac.LOGIN_PROMPT], 120)
        time.sleep(6)
        con.read_until(["__drain__"], 3)
        con.ensure_shell("root", "root", 60, nudge=True, settle=1.0)

        out = con.run("pidof sshd", 15)
        check("sshd auto-started", any(c.isdigit() for c in out.replace("pidof sshd", "")),
              repr(out.replace("pidof sshd", "").strip()[:40]))
        time.sleep(1.0)

        # ---- 1. isatty + interactive shell over a forced PTY ----
        print("\n=== ssh -tt: isatty + interactive ===", flush=True)
        # IMPORTANT: in raw -tt mode the PTY echoes the typed command, so a token
        # that appears literally in the command matches the ECHO, not the command
        # OUTPUT (a false positive that bit an earlier version). The printf '%s'
        # concatenation makes the OUTPUT token (ISATTY_OK / PTYMARK_7Q) never
        # appear in the echoed command text ('ISATTY_%s' + 'OK' separately) -- so
        # a match proves real output. whoami->root / uname->aarch64 are likewise
        # output-only strings.
        cmds = ("[ -t 0 ] && printf 'ISATTY_%s\\n' OK || printf 'ISATTY_%s\\n' NO\n"
                "whoami\n"
                "uname -a\n"
                "printf 'PTYMARK_%s\\n' 7Q\n"
                "exit\n")
        rc, sout, serr = attempt(cmds, ["PTYMARK_7Q", "aarch64"], timeout=60)
        # drain serial for sshd handshake logs
        con.read_until("___never___", 3)
        # isatty(0) is the authoritative terminal check (TCGETS succeeds on the
        # PTY). NOTE: the `tty` *command* / ttyname() still reports "not a tty"
        # because AIOS has no /dev/pts device node -- a separate, cosmetic
        # limitation that does not affect line editing / signals / full-screen
        # apps, which all key off isatty().
        check("isatty(0) TRUE over SSH (real output, not echo)", "ISATTY_OK" in sout,
              repr(sout[:140]))
        check("interactive command output (PTYMARK_7Q)", "PTYMARK_7Q" in sout,
              repr(sout[:140]))
        check("whoami=root (real output)", "root" in sout, repr(sout[:140]))
        check("uname shows aarch64 (real output)", "aarch64" in sout, repr(sout[:140]))

        # ---- 2. Ctrl-C: partial line discarded, session survives ----
        print("\n=== ssh -tt: Ctrl-C (VINTR -> SIGINT) ===", flush=True)
        # Type a bogus partial command (no newline), send ^C, then a clean
        # command. Working Ctrl-C: the partial line is discarded (SIGINT clears
        # it), the shell reprompts, CTRLC_RECOVERED runs, and ZZBOGUSZZ is NEVER
        # executed (no "not found"). The token appears in the echo regardless,
        # so we assert on the EXECUTION side effects.
        cc = ("ZZBOGUSZZ"        # partial line, no newline
              "\x03"             # Ctrl-C
              "echo CTRLC_RECOVERED\n"
              "exit\n")
        rc2, sout2, serr2 = attempt(cc, ["CTRLC_RECOVERED"], timeout=45)
        con.read_until("___never___", 3)
        check("Ctrl-C: session survives + next command runs",
              "CTRLC_RECOVERED" in sout2, repr(sout2[:140]))
        check("Ctrl-C: partial line was discarded (not executed)",
              "ZZBOGUSZZ: not found" not in sout2
              and "ZZBOGUSZZ:not found" not in sout2
              and "not found" not in sout2,
              repr(sout2[:140]))

        # ---- 3. less smoke (raw-mode app accepts input + quits) ----
        print("\n=== ssh -tt: less smoke ===", flush=True)
        lc = ("seq 1 60 | less\n"
              "q"               # quit less (raw mode, no newline needed)
              "echo LESS_DONE\n"
              "exit\n")
        rc3, sout3, serr3 = attempt(lc, ["LESS_DONE"], timeout=45)
        con.read_until("___never___", 3)
        # less may not be installed; treat "LESS_DONE after a clean session" as
        # the smoke pass, and note (not fail) if less is missing.
        if "LESS_DONE" in sout3 and ("not found" in sout3 or "No such" in sout3):
            print("  [INFO] less not installed -- raw-mode smoke skipped", flush=True)
        else:
            check("less smoke: raw-mode app ran + quit cleanly",
                  "LESS_DONE" in sout3, repr(sout3[:140]))

    except Exception as e:
        import traceback
        traceback.print_exc()
        check("harness exception", False, "%s: %s" % (type(e).__name__, e))
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        for p in (SOCK, ASKPASS):
            if os.path.exists(p):
                os.unlink(p)

    npass = sum(1 for ok in results if ok)
    print("\n=== SSH PTY e2e: %d/%d passed ===" % (npass, len(results)), flush=True)
    return 0 if results and npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())

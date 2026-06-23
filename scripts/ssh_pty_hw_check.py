#!/usr/bin/env python3
"""HW validation of the PTY-backed SSH console against a real AIOS board.

Unlike ssh_pty_qemu_test.py (which launches QEMU), this drives the REAL board
over the network with `ssh -tt` (force remote PTY). Because the board now runs
a fully coherent userspace (all binaries on the new libaios), it also checks
EXTERNAL commands (whoami/uname) -- a child of the shell inheriting the PTY via
the y<inst>: exec token -- not just dash builtins.

  python3 scripts/ssh_pty_hw_check.py [--host 192.168.0.8] [--port 2222]
"""
import argparse
import os
import subprocess
import sys
import time

ASKPASS = "/tmp/aios-hw-askpass.sh"
PASSWORD = "root"


def write_askpass():
    with open(ASKPASS, "w") as f:
        f.write("#!/bin/sh\necho '%s'\n" % PASSWORD)
    os.chmod(ASKPASS, 0o755)


def ssh_pty_run(host, port, commands, timeout=45):
    env = dict(os.environ)
    env["SSH_ASKPASS"] = ASKPASS
    env["SSH_ASKPASS_REQUIRE"] = "force"
    env["DISPLAY"] = env.get("DISPLAY", ":0")
    cmd = ["ssh", "-tt", "-p", str(port),
           "-o", "StrictHostKeyChecking=no",
           "-o", "UserKnownHostsFile=/dev/null",
           "-o", "PreferredAuthentications=password",
           "-o", "PubkeyAuthentication=no",
           "-o", "KbdInteractiveAuthentication=no",
           "-o", "NumberOfPasswordPrompts=1",
           "-o", "ConnectTimeout=10",
           "-o", "LogLevel=ERROR",
           "root@%s" % host]
    try:
        # CR line endings: real-terminal Enter is CR, and zsh's raw-mode ZLE
        # binds accept-line to CR (not LF). dash cooked mode (ICRNL) accepts CR.
        commands = commands.replace("\n", "\r")
        r = subprocess.run(cmd, input=commands.encode(), capture_output=True,
                           env=env, timeout=timeout, start_new_session=True)
        return r.returncode, r.stdout.decode("utf-8", "replace"), \
            r.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as e:
        return None, (e.stdout or b"").decode("utf-8", "replace"), "TIMEOUT"


def attempt(host, port, commands, want, timeout=45, tries=4):
    rc, sout, serr = None, "", ""
    for i in range(tries):
        rc, sout, serr = ssh_pty_run(host, port, commands, timeout=timeout)
        print("  [ssh -tt try %d] rc=%s out=%r err=%r"
              % (i + 1, rc, sout[:200], serr[:100]), flush=True)
        if any(w in sout for w in want):
            break
        time.sleep(3.0)
    return rc, sout, serr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.0.8")
    ap.add_argument("--port", type=int, default=2222)
    a = ap.parse_args()
    write_askpass()
    results = []

    def check(name, ok, detail=""):
        results.append(bool(ok))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  -- " + detail) if detail else ""), flush=True)

    print("=== HW PTY validation: %s:%d ===" % (a.host, a.port), flush=True)

    # 1. isatty + interactive + EXTERNAL commands (new libaios everywhere).
    # printf '%s' concatenation: the OUTPUT token never appears in the echoed
    # command, so a match proves REAL command output (not the raw-mode echo).
    cmds = ("[ -t 0 ] && printf 'ISATTY_%s\\n' OK || printf 'ISATTY_%s\\n' NO\n"
            "whoami\n"
            "uname -a\n"
            "printf 'HWMARK_%s\\n' 4Z\n"
            "exit\n")
    rc, sout, serr = attempt(a.host, a.port, cmds, ["HWMARK_4Z", "aarch64"])
    check("isatty(0) TRUE over SSH (real output, not echo)", "ISATTY_OK" in sout, repr(sout[:140]))
    check("interactive command output (HWMARK_4Z)", "HWMARK_4Z" in sout, repr(sout[:140]))
    check("external cmd whoami=root (child inherits PTY via y-token)",
          "root" in sout, repr(sout[:140]))
    check("external cmd uname=aarch64 over PTY (real output)", "aarch64" in sout, repr(sout[:140]))

    # 2. Ctrl-C: partial line discarded, session survives
    cc = "ZZBOGUSZZ\x03echo CTRLC_RECOVERED\nexit\n"
    rc2, sout2, serr2 = attempt(a.host, a.port, cc, ["CTRLC_RECOVERED"])
    check("Ctrl-C: session survives + next command runs",
          "CTRLC_RECOVERED" in sout2, repr(sout2[:140]))
    check("Ctrl-C: partial line discarded (not executed)",
          "ZZBOGUSZZ: not found" not in sout2 and "not found" not in sout2,
          repr(sout2[:140]))

    # 3. vi/less render smoke (if installed): run, quit, confirm clean exit
    for app, quitseq in (("vi", "\x1b:q!\n"), ("less", "q")):
        lc = "echo L1 > /tmp/vtest.txt; echo L2 >> /tmp/vtest.txt\n" \
             "%s /tmp/vtest.txt\n%secho %s_DONE\nexit\n" % (app, quitseq, app.upper())
        rc3, sout3, serr3 = attempt(a.host, a.port, lc, ["%s_DONE" % app.upper()], tries=2)
        done = ("%s_DONE" % app.upper()) in sout3
        missing = ("not found" in sout3)
        if missing and done:
            print("  [INFO] %s not installed -- skipped" % app, flush=True)
        else:
            check("%s render smoke (raw-mode app ran + quit)" % app, done,
                  repr(sout3[:160]))

    npass = sum(1 for ok in results if ok)
    print("\n=== HW PTY validation: %d/%d passed ===" % (npass, len(results)), flush=True)
    if os.path.exists(ASKPASS):
        os.unlink(ASKPASS)
    return 0 if results and npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())

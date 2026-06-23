#!/usr/bin/env python3
"""Serial-console regression test -- the PTY-work safety net (v0.4.296).

The PTY series (docs/DESIGN_PTY_SSH.md) refactors tty_server into multiple
instances and threads a controlling-PTY instance through the whole POSIX shim.
Instance 0 is the serial/keyboard console and MUST stay byte-identical. This is
the green-every-step gate for that invariant:

  boot QEMU (serial on a unix socket) -> "AIOS login:" -> root / root ->
  at the shell, run `echo TTYWO''RKS` and verify BOTH
    * cooked ECHO: the typed command is echoed back by the line discipline
      (the echoed form keeps the quotes: TTYWO''RKS), and
    * EXEC: dash runs it and prints the clean result TTYWORKS (the quotes are
      consumed by the shell, so this token appears ONLY in command output --
      it cannot come from the echo).

A second command (`echo SECOND_OK`) confirms the prompt/line discipline keeps
working for more than one line. Pure stdlib; reuses scripts/aios_console.Console.
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
SOCK = "/tmp/aios-serial-regress.sock"
SMP = int(os.environ.get("AIOS_SMP", "4"))

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)


def qemu_cmd():
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", str(SMP), "-m", "2G",
           "-display", "none", "-monitor", "none", "-no-reboot",
           "-serial", "unix:%s,server" % SOCK, "-kernel", KERNEL]
    for i, path in enumerate([DISK, LOGDISK]):
        if os.path.exists(path):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (path, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    # user net so getty's boot services (netconsole/sntp) start as in production
    cmd += ["-netdev", "user,id=n0", "-device", "virtio-net-device,netdev=n0"]
    return cmd


def main():
    results = []

    def check(name, ok, detail=""):
        results.append(bool(ok))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  -- " + detail) if detail else ""), flush=True)

    if os.path.exists(SOCK):
        os.unlink(SOCK)
    proc = subprocess.Popen(qemu_cmd())
    con = None
    try:
        sock = ac.connect_qemu_socket(SOCK)
        con = ac.Console(sock.fileno(), echo=True)
        print("=== booting QEMU -smp %d, serial login ===" % SMP, flush=True)
        # Let the boot quiesce (sntp/netconsole spam) before logging in.
        con.read_until([ac.LOGIN_PROMPT], 120)
        time.sleep(5)
        con.read_until(["__drain__"], 3)  # drain pending boot output
        con.ensure_shell("root", "root", 60, nudge=True, settle=1.0)
        check("serial login reached a shell", True)

        # --- the echo + exec invariant ---
        out = con.run("echo TTYWO''RKS", 20)
        check("cooked ECHO (line discipline echoes the typed command)",
              "TTYWO''RKS" in out, repr(out.strip()[:80]))
        check("EXEC (dash runs the command, prints clean output)",
              "TTYWORKS" in out, repr(out.strip()[:80]))

        # --- second line still works (prompt + discipline persist) ---
        out2 = con.run("echo SECOND_OK", 20)
        check("second command executes", "SECOND_OK" in out2,
              repr(out2.strip()[:80]))

        # --- liveness: a pipeline still computes correctly ---
        out3 = con.run("seq 1 50 | wc -l", 25)
        check("pipe still works (seq 1 50|wc -l = 50)",
              any(line.strip() == "50" for line in out3.splitlines()),
              repr(out3.strip()[:80]))
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
        if os.path.exists(SOCK):
            os.unlink(SOCK)

    npass = sum(1 for ok in results if ok)
    print("\n=== serial console regression: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if results and npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""QEMU regression test for the v0.4.190 identity/privesc fix.

The fix gates PIPE_SET_IDENTITY on a root caller and makes getty drop privilege
in its forked child (so getty stays root across logins). The RISK is breaking
login, especially the user->root multi-login that a naive root-only gate would
wedge. This drives getty through:
  1. root login          -> whoami == root
  2. exit, USER login     -> whoami == user   (getty stayed root to auth this)
  3. exit, root login again -> whoami == root (user->root multi-login works)

If getty had wedged at uid!=0 after the user login, step 3 would fail to reach a
root shell -- that is the exact failure the restructure prevents.
"""
import importlib.util
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-id-test.sock"

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)


def qemu_cmd():
    cmd = [
        "qemu-system-aarch64",
        "-machine", "virt,virtualization=on",
        "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
        "-display", "none", "-monitor", "none", "-no-reboot",
        "-serial", "unix:%s,server" % SOCK,
        "-kernel", KERNEL,
    ]
    for i, path in enumerate([DISK, LOGDISK]):
        if os.path.exists(path):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (path, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    return cmd


def run_cmd(con, cmd, timeout=25):
    # Prompt-agnostic: a root shell shows "# ", a non-root user shell shows "$ ".
    con.sendline(cmd)
    _, buf = con.read_until(["# ", "$ "], timeout)
    return buf.replace(cmd, "")


def whoami(con):
    return run_cmd(con, "whoami", 15)


def run_idtest(con):
    # Read to idtest's own RESULT= marker (avoids racing a stale shell prompt).
    con.sendline("idtest")
    pat, buf = con.read_until(
        ["RESULT=ROOT-OK", "RESULT=BLOCKED", "RESULT=VULNERABLE",
         "RESULT=ROOT-DENIED-UNEXPECTED"], 35)
    return pat or "", buf


def main():
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  -- " + detail) if detail else ""), flush=True)

    if os.path.exists(SOCK):
        os.unlink(SOCK)
    proc = subprocess.Popen(qemu_cmd())
    con = None
    try:
        sock = ac.connect_qemu_socket(SOCK)
        con = ac.Console(sock.fileno(), echo=True)
        print("=== boot ===", flush=True)
        con.read_until(["AIOS login:"], 120)
        time.sleep(4)
        con.read_until(["__drain__"], 3)

        # 1. root login
        print("\n=== 1. root login ===", flush=True)
        con.ensure_shell("root", "root", 50, nudge=True, settle=1.0)
        w = whoami(con)
        check("root login -> whoami root", "root" in w and "user" not in w, repr(w.strip()[-30:]))

        # 1b. idtest as root: the mkdir-/etc probe must succeed (discriminator).
        pat, buf = run_idtest(con)
        check("idtest as root -> ROOT-OK (probe valid)", pat == "RESULT=ROOT-OK",
              repr((pat + " | " + buf.strip())[-70:]))

        # 2. exit, user login
        print("\n=== 2. exit -> user login ===", flush=True)
        con.sendline("exit")
        con.read_until(["AIOS login:"], 30)
        time.sleep(1.0)
        con.ensure_shell("user", "user", 50, nudge=True, settle=1.0)
        w = whoami(con)
        check("user login -> whoami user", "user" in w, repr(w.strip()[-30:]))

        # 2b. THE EXPLOIT, as the non-root user: PIPE_SET_IDENTITY(0) must be
        # denied (rc=-1) and the root-only mkdir must stay blocked.
        pat, buf = run_idtest(con)
        check("privesc denied: PIPE_SET_IDENTITY rc=-1", "rc = -1" in buf,
              repr(buf.strip()[-80:]))
        check("privesc blocked: no root fs access as user", pat == "RESULT=BLOCKED",
              repr((pat + " | " + buf.strip())[-70:]))

        # 3. exit, root login again (the user->root multi-login)
        print("\n=== 3. exit -> root login again (multi-login) ===", flush=True)
        con.sendline("exit")
        con.read_until(["AIOS login:"], 30)
        time.sleep(1.0)
        relog = True
        try:
            con.ensure_shell("root", "root", 50, nudge=True, settle=1.0)
        except Exception as e:
            relog = False
            print("  re-login error: %r" % e, flush=True)
        w = whoami(con) if relog else ""
        check("root re-login after user -> whoami root",
              relog and "root" in w and "user" not in w, repr(w.strip()[-30:]))

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

    npass = sum(1 for _, ok, _ in results if ok)
    print("\n=== identity/login QEMU test: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())

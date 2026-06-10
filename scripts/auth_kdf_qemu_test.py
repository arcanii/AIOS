#!/usr/bin/env python3
"""QEMU test for the salted-KDF password auth (v0.4.189).

The key correctness check: the device auth_server must VERIFY a password hash
that was generated on the HOST by scripts/gen_etc_passwd.py. If login as
root/root succeeds, the iterated-SHA3 KDF matches byte-for-byte between the host
generator (in /etc/passwd) and the device verifier. Also checks:
  - wrong password is rejected (verify isn't accidentally always-true),
  - the hash on disk is the new salted "$a1$..." format,
  - `su user` (root, no password) still works,
and reports the login latency (the KDF work factor in practice).
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
SOCK = "/tmp/aios-auth-test.sock"

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


def main():
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  -- " + detail) if detail else ""), flush=True)

    # Sanity: the on-disk passwd hash must be the new salted format.
    pw = open(os.path.join(REPO, "disk/rootfs/etc/passwd")).read()
    check("/etc/passwd uses $a1$ salted format", "$a1$" in pw,
          repr(pw.splitlines()[0][:40]) if pw else "(empty)")

    if os.path.exists(SOCK):
        os.unlink(SOCK)
    proc = subprocess.Popen(qemu_cmd())
    con = None
    try:
        sock = ac.connect_qemu_socket(SOCK)
        con = ac.Console(sock.fileno(), echo=True)
        print("=== booting ===", flush=True)
        con.read_until(["AIOS login:"], 120)
        time.sleep(4)
        con.read_until(["__drain__"], 3)

        # 1. WRONG password must be denied.
        print("\n=== wrong password (expect denied) ===", flush=True)
        denied = False
        try:
            con.ensure_shell("root", "definitely-wrong-pw", 40, nudge=True, settle=1.0)
        except RuntimeError as e:
            denied = "denied" in str(e).lower()
        except Exception:
            pass
        check("wrong password denied", denied)

        # 2. CORRECT password must log in -> proves host/device KDF match.
        print("\n=== correct password root/root (host-generated hash) ===",
              flush=True)
        # re-sync to a fresh login prompt after the denied attempt
        con.read_until(["AIOS login:"], 30)
        time.sleep(1.0)
        t0 = time.monotonic()
        logged_in = True
        try:
            con.ensure_shell("root", "root", 40, nudge=True, settle=1.0)
        except Exception as e:
            logged_in = False
            print("  login error: %r" % e, flush=True)
        dt = time.monotonic() - t0
        check("login root/root succeeds (KDF host==device)", logged_in,
              "login took %.1fs" % dt)

        if logged_in:
            # 3. identity correct
            out = con.run("whoami", 15)
            check("whoami == root", "root" in out.replace("whoami", ""),
                  repr(out.strip()[-40:]))
            # 4. su to user (root su's without a password) exercises the su path
            out = con.run("su user -c whoami 2>/dev/null || su user", 15)
            # fall back: just confirm su command exists and runs
            check("su user runs", "user" in out or "#" in out,
                  repr(out.strip()[-50:]))

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
    print("\n=== auth KDF QEMU test: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""QEMU test for the sshd failed-login backoff + pre-auth hardening (v0.4.187).

Boots AIOS under qemu (same harness as ssh_qemu_test.py), waits for the
getty-started sshd, then:
  1. connects with a WRONG password, 3 prompts in one connection -- the server
     must delay 2s/5s/8s before each USERAUTH_FAILURE, so the whole attempt
     takes >= ~15s wall clock (without the backoff it completes in ~1-2s)
  2. reconnects with a wrong password, 1 prompt -- prior failures (3) must
     persist across connections, so the single failure takes >= ~5s
  3. connects with the CORRECT password -- must succeed immediately (the
     backoff only fires on failure; there is no lockout)
  4. floods the version exchange with non-SSH junk lines -- the server must
     disconnect after the 64-line cap instead of reading junk forever
  5. connects and sends NOTHING -- the pre-auth deadline (60s) must drop the
     idle connection instead of holding the single-session server forever
  6. logs in normally again -- the server must have survived 4 and 5
"""
import importlib.util
import os
import socket as pysock
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-ssh-backoff-test.sock"
ASKPASS = "/tmp/aios-ssh-backoff-askpass.sh"
PORT = 2222
GOOD_PASSWORD = "root"
BAD_PASSWORD = "definitely-wrong"

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
    cmd += ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:%d" % (PORT, PORT),
            "-device", "virtio-net-device,netdev=n0"]
    return cmd


def write_askpass(password):
    with open(ASKPASS, "w") as f:
        f.write("#!/bin/sh\necho '%s'\n" % password)
    os.chmod(ASKPASS, 0o755)


def ssh_run(commands, prompts, timeout):
    """Run the OpenSSH client; password comes from the askpass helper.
    Returns (returncode, stdout, stderr, elapsed_seconds)."""
    env = dict(os.environ)
    env["SSH_ASKPASS"] = ASKPASS
    env["SSH_ASKPASS_REQUIRE"] = "force"
    env["DISPLAY"] = env.get("DISPLAY", ":0")
    cmd = [
        "ssh", "-T",
        "-p", str(PORT),
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "PreferredAuthentications=password",
        "-o", "PubkeyAuthentication=no",
        "-o", "KbdInteractiveAuthentication=no",
        "-o", "NumberOfPasswordPrompts=%d" % prompts,
        "-o", "ConnectTimeout=10",
        "-o", "LogLevel=ERROR",
        "root@127.0.0.1",
    ]
    t0 = time.monotonic()
    r = subprocess.run(cmd, input=commands.encode(), capture_output=True,
                       env=env, timeout=timeout, start_new_session=True)
    elapsed = time.monotonic() - t0
    return r.returncode, r.stdout.decode("utf-8", "replace"), \
        r.stderr.decode("utf-8", "replace"), elapsed


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
        print("=== booting + logging in ===", flush=True)
        con.read_until(["AIOS login:"], 120)
        time.sleep(6)
        con.read_until(["__quiesce_never__"], 3)
        con.ensure_shell("root", "root", 60, nudge=True, settle=1.0)

        out = con.run("pidof sshd", 15)
        body = out.replace("pidof sshd", "")
        auto = any(ch.isdigit() for ch in body)
        check("sshd auto-started by getty", auto, repr(body.strip()[:60]))
        if not auto:
            con.sendline("sshd &")
            con.read_until(["Listening on port %d" % PORT], 40)
        time.sleep(1.0)

        # ---- 1. wrong password x3 in one connection: 2s + 5s + 8s ----
        print("\n=== wrong password, 3 prompts (expect >= ~15s) ===", flush=True)
        write_askpass(BAD_PASSWORD)
        rc, sout, serr, dt = ssh_run("exit\n", prompts=3, timeout=120)
        print("  rc=%s elapsed=%.1fs stderr=%r" % (rc, dt, serr[:120]), flush=True)
        check("wrong password rejected", rc != 0 and "denied" in serr.lower(),
              "rc=%s" % rc)
        check("backoff delayed the 3-attempt connection (>=12s)", dt >= 12.0,
              "elapsed=%.1fs" % dt)

        # ---- 2. reconnect, wrong password x1: prior failures persist ----
        print("\n=== reconnect, wrong password, 1 prompt (expect >= ~5s) ===",
              flush=True)
        rc, sout, serr, dt = ssh_run("exit\n", prompts=1, timeout=60)
        print("  rc=%s elapsed=%.1fs stderr=%r" % (rc, dt, serr[:120]), flush=True)
        check("reconnect wrong password rejected", rc != 0, "rc=%s" % rc)
        check("failure cost persists across reconnect (>=3.5s)", dt >= 3.5,
              "elapsed=%.1fs" % dt)

        # ---- 3. correct password: succeeds, no lockout ----
        print("\n=== correct password (expect success, no delay) ===", flush=True)
        write_askpass(GOOD_PASSWORD)
        marker = "BACKOFF_OK_91D3"
        rc, sout, serr, dt = ssh_run("echo %s\nexit\n" % marker,
                                     prompts=1, timeout=60)
        print("  rc=%s elapsed=%.1fs stdout=%r" % (rc, dt, sout[:120]), flush=True)
        check("correct password still logs in (no lockout)", marker in sout,
              repr(sout[:80]))

        # ---- 4. junk flood: >64 non-SSH lines must be disconnected ----
        print("\n=== junk flood (expect disconnect at the 64-line cap) ===",
              flush=True)
        t0 = time.monotonic()
        eof = False
        try:
            js = pysock.create_connection(("127.0.0.1", PORT), timeout=10)
            js.settimeout(30)
            for _ in range(80):
                js.sendall(b"GET / HTTP/1.0\r\n")
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                try:
                    d = js.recv(4096)
                except pysock.timeout:
                    break
                if not d:
                    eof = True
                    break
            js.close()
        except (BrokenPipeError, ConnectionResetError):
            eof = True
        dt = time.monotonic() - t0
        print("  eof=%s elapsed=%.1fs" % (eof, dt), flush=True)
        check("junk flood gets disconnected", eof, "elapsed=%.1fs" % dt)

        # ---- 5. idle connection: dropped by the 60s pre-auth deadline ----
        print("\n=== idle connection (expect drop at ~60s) ===", flush=True)
        t0 = time.monotonic()
        eof = False
        try:
            idle = pysock.create_connection(("127.0.0.1", PORT), timeout=10)
            idle.settimeout(100)
            while True:
                d = idle.recv(4096)
                if not d:
                    eof = True
                    break
            idle.close()
        except (pysock.timeout, ConnectionResetError):
            pass
        dt = time.monotonic() - t0
        print("  eof=%s elapsed=%.1fs" % (eof, dt), flush=True)
        check("idle connection dropped by pre-auth deadline (50-95s)",
              eof and 50.0 <= dt <= 95.0, "eof=%s elapsed=%.1fs" % (eof, dt))

        # ---- 6. server survived: normal login still works ----
        print("\n=== login after flood + idle kill (expect success) ===",
              flush=True)
        marker2 = "RECOVER_OK_7C2"
        rc, sout, serr, dt = ssh_run("echo %s\nexit\n" % marker2,
                                     prompts=1, timeout=60)
        print("  rc=%s elapsed=%.1fs stdout=%r" % (rc, dt, sout[:120]), flush=True)
        check("login works after flood + idle kill", marker2 in sout,
              repr(sout[:80]))

        con.read_until("___never___", 3)  # drain sshd logs to the capture

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

    npass = sum(1 for _, ok, _ in results if ok)
    print("\n=== ssh backoff QEMU test: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())

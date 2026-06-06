#!/usr/bin/env python3
"""QEMU smoke test for the AIOS SSH server (sshd, port 2222).

Boots AIOS under qemu with user-mode net + a hostfwd for 2222, logs in over the
serial unix socket (reusing aios_console.Console), runs the mbedTLS runtime smoke
(test_mbedtls), starts "sshd &", then connects with the real OpenSSH client from
the host and drives an interactive shell. Validates the full v0.4.84 path:
  - libmbedcrypto.a links + runs on AIOS (AES / SHA-256 / CTR-DRBG)
  - sshd binds + listens on 2222
  - version exchange -> KEXINIT -> ECDH key exchange -> NEWKEYS
  - encrypted transport + password auth (AUTH_LOGIN via auth_server)
  - session channel + shell spawn (fork+exec /bin/dash) + bidirectional relay

The password is fed to ssh via SSH_ASKPASS_REQUIRE=force (no tty needed): we run
ssh in its own session (start_new_session) with no controlling terminal, so
OpenSSH calls our askpass helper for the password instead of /dev/tty.

QEMU SLIRP has no TCP retransmit, so a session can occasionally drop mid-stream
(documented). We retry the ssh connection a few times before failing.
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
SOCK = "/tmp/aios-ssh-test.sock"
ASKPASS = "/tmp/aios-ssh-askpass.sh"
PORT = 2222
PASSWORD = "root"

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


def write_askpass():
    with open(ASKPASS, "w") as f:
        f.write("#!/bin/sh\necho '%s'\n" % PASSWORD)
    os.chmod(ASKPASS, 0o755)


def ssh_run(commands, timeout=60):
    """Run the OpenSSH client against AIOS, feeding `commands` to the shell.
    Returns (returncode, stdout, stderr). Password supplied via askpass."""
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
        "-o", "NumberOfPasswordPrompts=1",
        "-o", "ConnectTimeout=10",
        "-o", "LogLevel=ERROR",
        "root@127.0.0.1",
    ]
    r = subprocess.run(cmd, input=commands.encode(), capture_output=True,
                       env=env, timeout=timeout, start_new_session=True)
    return r.returncode, r.stdout.decode("utf-8", "replace"), \
        r.stderr.decode("utf-8", "replace")


def main():
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
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
        print("=== booting + logging in ===", flush=True)
        # getty forks 3 services at boot (netconsole, sshd, sntp); their
        # startup output + kernel exec logs interleave with the login/password
        # prompts. Let the boot quiesce, drain it, then log in on a clean prompt
        # nudged by a newline (avoids the contiguous-"Password:" match flaking).
        con.read_until(["AIOS login:"], 120)
        time.sleep(6)
        con.read_until(["__quiesce_never__"], 3)  # drain pending boot output
        con.ensure_shell("root", "root", 60, nudge=True, settle=1.0)

        # ---- runtime crypto smoke (validates libmbedcrypto.a on AIOS) ----
        print("\n=== mbedTLS runtime smoke (test_mbedtls) ===", flush=True)
        out = con.run("test_mbedtls", 30)
        check("mbedtls smoke PASS on AIOS", "mbedtls smoke: PASS" in out,
              repr(out.strip().splitlines()[-1] if out.strip() else ""))

        # ---- sshd: getty auto-starts it at boot (v0.4.177). Verify that;
        #      fall back to a manual launch for an older disk without it. ----
        print("\n=== sshd (getty auto-start) ===", flush=True)
        out = con.run("pidof sshd", 15)
        body = out.replace("pidof sshd", "")
        auto = any(ch.isdigit() for ch in body)
        check("sshd auto-started by getty", auto, repr(body.strip()[:60]))
        if not auto:
            print("  (not auto-started -- launching manually)", flush=True)
            con.sendline("sshd &")
            pat, _ = con.read_until(["Listening on port %d" % PORT,
                                     "Crypto init failed",
                                     "bind(%d) failed" % PORT], 40)
            if pat is None or "Listening" not in (pat or ""):
                check("sshd listening on %d" % PORT, False, repr(pat))
                return 1
        time.sleep(1.0)  # let the accept loop settle

        # netconsole must still auto-start too (regression check)
        out = con.run("pidof netconsole", 15)
        check("netconsole still auto-started",
              any(ch.isdigit() for ch in out.replace("pidof netconsole", "")),
              repr(out.replace("pidof netconsole", "").strip()[:60]))

        # ---- connect from the host with real OpenSSH ----
        print("\n=== ssh from host (interactive shell) ===", flush=True)
        marker = "SSH_MARKER_5A17C"
        commands = ("uname -a\n"
                    "whoami\n"
                    "echo %s\n"
                    "ls /bin | head -3\n"
                    "exit\n") % marker
        rc, sout, serr = (None, "", "")
        for attempt in range(3):
            try:
                rc, sout, serr = ssh_run(commands, timeout=60)
            except subprocess.TimeoutExpired:
                sout, serr = "", "timeout"
            print("  [ssh attempt %d] rc=%s stdout=%r stderr=%r"
                  % (attempt + 1, rc, sout[:200], serr[:200]), flush=True)
            if marker in sout or "AIOS" in sout or "root" in sout:
                break
            time.sleep(2.0)

        # drain serial to capture sshd handshake logs
        con.read_until("___never___", 3)

        check("ssh: shell echoed marker", marker in sout, repr(sout[:120]))
        check("ssh: whoami=root", "root" in sout, repr(sout[:120]))
        check("ssh: uname shows AIOS", "AIOS" in sout, repr(sout[:120]))

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
    print("\n=== sshd QEMU smoke: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())

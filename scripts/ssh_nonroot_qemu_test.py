#!/usr/bin/env python3
"""QEMU test: non-root SSH login gets the correct identity (v0.4.190).

After the privesc fix, sshd stays root and drops privilege in the forked shell
child. This verifies a NON-ROOT SSH login (user/user) yields a shell whose
identity is actually `user` -- the case the all-root ssh tests never exercised,
and exactly the regression the gate could have introduced if sshd mutated its
own slot.
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
SOCK = "/tmp/aios-ssh-nonroot.sock"
ASKPASS = "/tmp/aios-ssh-nonroot-askpass.sh"
PORT = 2222

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)


def qemu_cmd():
    cmd = [
        "qemu-system-aarch64", "-machine", "virt,virtualization=on",
        "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
        "-display", "none", "-monitor", "none", "-no-reboot",
        "-serial", "unix:%s,server" % SOCK, "-kernel", KERNEL,
    ]
    for i, path in enumerate([DISK, LOGDISK]):
        if os.path.exists(path):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (path, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    cmd += ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:%d" % (PORT, PORT),
            "-device", "virtio-net-device,netdev=n0"]
    return cmd


def ssh_run(commands, password, timeout=60):
    env = dict(os.environ)
    env["SSH_ASKPASS"] = ASKPASS
    env["SSH_ASKPASS_REQUIRE"] = "force"
    env["DISPLAY"] = env.get("DISPLAY", ":0")
    with open(ASKPASS, "w") as f:
        f.write("#!/bin/sh\necho '%s'\n" % password)
    os.chmod(ASKPASS, 0o755)
    cmd = ["ssh", "-tt", "-p", str(PORT),
           "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
           "-o", "PreferredAuthentications=password", "-o", "PubkeyAuthentication=no",
           "-o", "KbdInteractiveAuthentication=no", "-o", "NumberOfPasswordPrompts=1",
           "-o", "ConnectTimeout=10", "-o", "LogLevel=ERROR", "user@127.0.0.1"]
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
    proc = subprocess.Popen(qemu_cmd())
    con = None
    try:
        sock = ac.connect_qemu_socket(SOCK)
        con = ac.Console(sock.fileno(), echo=True)
        print("=== boot + console login ===", flush=True)
        con.read_until(["AIOS login:"], 120)
        time.sleep(6)
        con.read_until(["__drain__"], 3)
        con.ensure_shell("root", "root", 60, nudge=True, settle=1.0)
        out = con.run("pidof sshd", 15)
        if not any(c.isdigit() for c in out.replace("pidof sshd", "")):
            con.sendline("sshd &")
            con.read_until(["Listening on port %d" % PORT], 40)
        time.sleep(1.0)

        print("\n=== SSH login as user/user ===", flush=True)
        rc, sout, serr = (None, "", "")
        for attempt in range(3):
            try:
                rc, sout, serr = ssh_run("whoami\nexit\n", "user", 60)
            except subprocess.TimeoutExpired:
                sout, serr = "", "timeout"
            print("  [attempt %d] rc=%s out=%r err=%r"
                  % (attempt + 1, rc, sout[:160], serr[:120]), flush=True)
            if "user" in sout:
                break
            time.sleep(2.0)
        con.read_until(["__drain__"], 3)
        # whoami must be 'user' (not root) -- proves the shell child dropped to
        # the non-root identity; the "$ " prompt (vs root "# ") corroborates.
        check("non-root SSH: whoami == user",
              "user" in sout and "root" not in sout.replace("user", ""),
              repr(sout[:120]))
        check("non-root SSH: non-root shell prompt ($)", "$ " in sout,
              repr(sout[:120]))

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
    print("\n=== non-root SSH QEMU test: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())

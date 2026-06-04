#!/usr/bin/env python3
"""QEMU smoke test for netconsole v2 (command-per-connection TCP shell).

Boots AIOS under qemu with user-mode net + a hostfwd for port 2323, logs in
over the serial unix socket (reusing aios_console.Console), starts
"netconsole &", then drives it over a real TCP socket from the host. Validates:
  - banner + prompt on connect
  - a command with output (echo)
  - a pipe inside dash -c (ls | head)
  - a NO-OUTPUT command (true) returns promptly -- the post-v0.4.143 EOF case
  - multi-line output (cat /proc/cmdline)
  - exit closes the connection
  - a fresh connection is served (accept loop)
"""
import importlib.util
import os
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) \
    if "/scripts/" in os.path.abspath(__file__) else \
    "/Users/bryan/Desktop/github_repos/AIOS"
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-netcon-test.sock"
PORT = 2323

# Import Console + helpers from aios_console.py without running its main().
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


class Tcp:
    """Minimal expect-style TCP client for the netconsole protocol."""
    def __init__(self, host, port, timeout=20):
        self.s = socket.create_connection((host, port), timeout=10)
        self.s.settimeout(1.0)
        self.buf = b""
        self.timeout = timeout

    def expect(self, pat, timeout=None):
        pat = pat.encode() if isinstance(pat, str) else pat
        deadline = time.time() + (timeout or self.timeout)
        while True:
            i = self.buf.find(pat)
            if i != -1:
                out = self.buf[:i + len(pat)]
                self.buf = self.buf[i + len(pat):]
                return out.decode("utf-8", "replace")
            if time.time() > deadline:
                raise TimeoutError(
                    "TCP expect %r timed out; buffered=%r" % (pat, self.buf))
            try:
                d = self.s.recv(4096)
            except socket.timeout:
                continue
            if not d:
                raise EOFError("server closed; buffered=%r" % self.buf)
            self.buf += d

    def closed(self, timeout=5):
        """Return True if the server closes the connection within timeout."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                d = self.s.recv(4096)
            except socket.timeout:
                continue
            if not d:
                return True
            self.buf += d
        return False

    def sendline(self, s):
        self.s.sendall((s + "\n").encode())

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


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
        con.ensure_shell("root", "root", 120)

        # netconsole auto-starts at boot (getty forks it) -- no manual launch.
        # Confirm it is reachable over TCP, retrying briefly in case it is still
        # binding right after login.
        print("=== connecting to auto-started netconsole ===", flush=True)
        t = None
        for _ in range(20):
            try:
                t = Tcp("127.0.0.1", PORT)
                break
            except OSError:
                time.sleep(0.5)
        check("netconsole auto-started + reachable", t is not None)
        if t is None:
            return 1

        # ---- connection 1 ----
        print("\n=== TCP connection 1 ===", flush=True)
        banner = t.expect("aios# ")
        check("banner on connect", "netconsole" in banner, repr(banner[:60]))

        t.sendline("echo hello-netcon")
        out = t.expect("aios# ")
        check("echo output", "hello-netcon" in out, repr(out.strip()[:80]))

        t.sendline("ls /bin | head -3")
        out = t.expect("aios# ")
        check("pipe inside dash -c", len(out.replace("aios# ", "").strip()) > 0,
              repr(out.replace("aios# ", "").strip()[:80]))

        # The crux: a no-output command must return a prompt promptly (clean
        # EOF on dash exit), NOT hang until the command timeout.
        t0 = time.time()
        t.sendline("true")
        out = t.expect("aios# ", timeout=10)
        dt = time.time() - t0
        check("no-output command returns promptly", dt < 8,
              "%.2fs, out=%r" % (dt, out.replace("aios# ", "")))

        t.sendline("cat /proc/cmdline")
        out = t.expect("aios# ")
        check("multi-line output (/proc/cmdline)",
              len(out.replace("aios# ", "").strip()) > 0,
              repr(out.replace("aios# ", "").strip()[:80]))

        t.sendline("exit")
        check("exit closes connection", t.closed(8))
        t.close()

        # ---- connection 2 (accept loop serves repeated clients) ----
        # No settle: reconnect immediately to exercise the accept-backlog drain
        # (connection completes its handshake before netconsole re-enters accept).
        print("\n=== TCP connection 2 (immediate reconnect) ===", flush=True)
        t2 = Tcp("127.0.0.1", PORT)
        t2.expect("aios# ")
        t2.sendline("echo second-conn")
        out = t2.expect("aios# ")
        check("second connection served", "second-conn" in out, repr(out.strip()[:80]))
        t2.sendline("exit")
        t2.closed(5)
        t2.close()

        # ---- reconnect stress: many cycles must not leak socket slots ----
        # MAX_NET_SOCKETS is 8 (1 listener + conns); if closed connections did
        # not free their slot, accept() would starve after a few rounds.
        print("\n=== reconnect stress (10 cycles) ===", flush=True)
        stress_ok = 0
        for i in range(10):
            try:
                tn = Tcp("127.0.0.1", PORT)
                tn.expect("aios# ", timeout=12)
                tn.sendline("echo cycle-%d" % i)
                out = tn.expect("aios# ", timeout=12)
                if ("cycle-%d" % i) in out:
                    stress_ok += 1
                tn.sendline("exit")
                tn.closed(5)
                tn.close()
            except Exception as e:
                print("  cycle %d failed: %s" % (i, e), flush=True)
                break
        check("reconnect stress 10/10", stress_ok == 10, "%d/10 cycles" % stress_ok)

    except Exception as e:
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
    print("\n=== netconsole QEMU smoke: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())

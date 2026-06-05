#!/usr/bin/env python3
"""QEMU test for netconsole2 (v2 DEBUG, port 2324, manual-start).

Boots AIOS, logs in over serial, launches /bin/netconsole2 backgrounded from the
serial login shell (the shell stays alive, so netconsole2 is NOT orphaned), then
drives it over TCP 2324. Validates that the non-blocking multi-session event loop
plus the guarded nonblock-accept work end-to-end, that the socket-close fix did
not break the happy path, and -- most important for the HW debug plan -- that the
/tmp/nc2.trace mechanism writes and is readable and that now_ms advances.

QEMU CANNOT reproduce the hardware relay stall (the whole reason netconsole2 is
serial-debugged on the Pi). This test only confirms netconsole2 is not broken on
the happy path BEFORE we flash, so the one flash is spent on a known-good binary.
"""
import importlib.util
import os
import socket
import subprocess
import sys
import time

REPO = "/Users/bryan/Desktop/github_repos/AIOS"
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-nc2-test.sock"
PORT = 2324

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
    cmd += ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:2323-:2323,"
            "hostfwd=tcp:127.0.0.1:2324-:2324",
            "-device", "virtio-net-device,netdev=n0"]
    return cmd


class Tcp:
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
                raise TimeoutError("expect %r timed out; buf=%r" % (pat, self.buf))
            try:
                d = self.s.recv(4096)
            except socket.timeout:
                continue
            if not d:
                raise EOFError("server closed; buf=%r" % self.buf)
            self.buf += d

    def sendline(self, s):
        self.s.sendall((s + "\n").encode())

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def t_advances(trace):
    ts = []
    for ln in trace.splitlines():
        i = ln.rfind("t=")
        if i != -1:
            tok = ln[i + 2:].split()
            if tok and tok[0].lstrip("-").isdigit():
                ts.append(int(tok[0]))
    return len(ts) >= 2 and max(ts) > min(ts)


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
        print("=== boot + login ===", flush=True)
        con.ensure_shell("root", "root", 120)

        # Launch netconsole2 backgrounded from the login shell. The shell stays
        # alive, so netconsole2's parent persists (not orphaned). Its stdout goes
        # to the serial console (captured), the banner confirms it started.
        print("=== launch /bin/netconsole2 & ===", flush=True)
        con.run("/bin/netconsole2 &", 10)
        time.sleep(2.0)

        t = None
        for _ in range(20):
            try:
                t = Tcp("127.0.0.1", PORT)
                break
            except OSError:
                time.sleep(0.5)
        check("netconsole2 reachable on 2324", t is not None)
        if t is None:
            print(con.run("cat /tmp/nc2.trace", 5), flush=True)
            return 1

        banner = t.expect("aios# ")
        check("banner says netconsole2", "netconsole2" in banner, repr(banner[:60]))

        t.sendline("echo nc2-works")
        out = t.expect("aios# ")
        check("echo output relayed", "nc2-works" in out, repr(out.strip()[:80]))

        t.sendline("ls /bin | head -3")
        out = t.expect("aios# ")
        check("pipe inside dash -c", len(out.replace("aios# ", "").strip()) > 0,
              repr(out.replace("aios# ", "").strip()[:80]))

        t0 = time.time()
        t.sendline("true")
        out = t.expect("aios# ", timeout=10)
        check("no-output cmd prompts promptly", time.time() - t0 < 8,
              "%.2fs" % (time.time() - t0))

        # Second concurrent session while the first is idle (the v1 limitation v2
        # is meant to remove).
        t2 = Tcp("127.0.0.1", PORT)
        t2.expect("aios# ")
        t2.sendline("echo second2")
        o2 = t2.expect("aios# ")
        check("2nd concurrent session served", "second2" in o2, repr(o2.strip()[:60]))
        t2.sendline("exit")
        t2.close()

        # The crux for the HW plan: confirm the trace mechanism works and is
        # readable (we pull it via __get on the Pi), and that now_ms advances.
        # Pull it over the netconsole2 TCP relay itself (reliable Tcp.expect),
        # not the serial shell, so the capture is deterministic.
        t.sendline("cat /tmp/nc2.trace")
        trace = t.expect("aios# ")
        check("trace file populated", "accept_ok" in trace and "fork_ok" in trace,
              "len=%d" % len(trace))
        check("trace shows run_data (relay delivered)", "run_data" in trace)
        check("now_ms advances on QEMU", t_advances(trace))
        print("---- trace tail ----", flush=True)
        print(trace[-1000:], flush=True)

        t.sendline("exit")
        t.close()

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
    print("\n=== netconsole2 QEMU: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())

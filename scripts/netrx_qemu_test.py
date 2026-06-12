#!/usr/bin/env python3
"""QEMU test: inbound TCP integrity under loss/retransmission (netstat knobs).

For the push-corruption hunt (docs/NEXT_20260612_net_rx_corruption.md):
~50% of 1.5MB HW pushes land with short 0xFF-biased corrupt runs while the
block layer's counters stay zero. This harness exercises the AIOS inbound
TCP path deterministically on QEMU:

  A. baseline 1.5MB __put + __get pull-back compare (counters must be quiet,
     and tcp_cksum_drops MUST be 0 -- catches SLIRP checksum surprises with
     the new inbound verification before it could ever false-drop on HW)
  B. /proc/netstat.drop.N fault injection (drop every Nth inbound TCP data
     segment) at increasing brutality -- forces SLIRP retransmissions, so
     the dup/overlap-trim/partial-window paths run constantly. The stream
     must stay byte-perfect; corruption here = reproduced HW bug.

Per qemu-test-hygiene: private disk copies, unique sockets/ports, no bare
pkill. Uses the serial console for /proc reads (keeps the netconsole TCP
stream pure for the transfer under test).
"""
import hashlib
import importlib.util
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-netrx-test.sock"
NC_PORT = 13231   # unique host port -> guest 2323

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)


def sh(con, cmd, timeout=30):
    out = con.run(cmd, timeout)
    if out == "":
        raise TimeoutError("console timed out: %r" % cmd)
    return out


class NC:
    """Minimal netconsole client against the hostfwd port."""
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", NC_PORT), timeout=20)
        self.s.settimeout(5)
        self.buf = b""

    def expect(self, pat, to):
        dl = time.time() + to
        while time.time() < dl:
            i = self.buf.find(pat)
            if i != -1:
                r = self.buf[:i+len(pat)]
                self.buf = self.buf[i+len(pat):]
                return r
            try:
                d = self.s.recv(262144)
                if d:
                    self.buf += d
            except socket.timeout:
                pass
        raise TimeoutError("expect %r; tail=%r" % (pat, self.buf[-200:]))

    def put(self, path, data, to=900):
        self.s.settimeout(60)
        self.s.sendall(("__put %s %d\n" % (path, len(data))).encode())
        self.s.sendall(data)
        r = self.expect(b"aios# ", to).split(b"aios# ")[0]
        self.s.settimeout(5)
        return r.decode("utf-8", "replace").strip()

    def get(self, path, to=600):
        self.s.sendall(("__get %s\n" % path).encode())
        hdr = self.expect(b"\n", 120).decode().strip()
        if not hdr.startswith("__get ok"):
            raise RuntimeError("get failed: %r" % hdr)
        n = int(hdr.split()[2])
        dl = time.time() + to
        while len(self.buf) < n and time.time() < dl:
            try:
                d = self.s.recv(262144)
                if d:
                    self.buf += d
            except socket.timeout:
                pass
        if len(self.buf) < n:
            raise TimeoutError("got %d/%d" % (len(self.buf), n))
        out = self.buf[:n]
        self.buf = self.buf[n:]
        self.expect(b"aios# ", 60)
        return out

    def close(self):
        try:
            self.s.sendall(b"exit\n")
            self.s.close()
        except OSError:
            pass


def netstat(con):
    out = sh(con, "cat /proc/netstat", 30)
    stats = {}
    for line in out.splitlines():
        parts = line.strip().split(":")
        if len(parts) == 2 and parts[1].strip().isdigit():
            stats[parts[0].strip()] = int(parts[1].strip())
    return stats


def diff_runs(got, want):
    diffs = [i for i in range(min(len(got), len(want))) if got[i] != want[i]]
    if not diffs and len(got) == len(want):
        return []
    runs = []
    st = pv = diffs[0]
    for i in diffs[1:]:
        if i == pv + 1:
            pv = i
        else:
            runs.append((st, pv))
            st = pv = i
    runs.append((st, pv))
    return runs


def main():
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  -- " + detail) if detail else ""), flush=True)

    payload = open(os.path.join(REPO, "disk/kernel8.img"), "rb").read()
    print("payload: %d bytes sha %s" % (len(payload),
          hashlib.sha256(payload).hexdigest()[:16]), flush=True)

    workdir = tempfile.mkdtemp(prefix="aios-netrx-")
    proc = None
    try:
        disks = []
        for src in [DISK, LOGDISK]:
            if os.path.exists(src):
                dst = os.path.join(workdir, os.path.basename(src))
                shutil.copyfile(src, dst)
                disks.append(dst)

        if os.path.exists(SOCK):
            os.unlink(SOCK)
        cmd = [
            "qemu-system-aarch64",
            "-machine", "virt,virtualization=on",
            "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
            "-display", "none", "-monitor", "none", "-no-reboot",
            "-serial", "unix:%s,server" % SOCK,
            "-kernel", KERNEL,
            "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:2323" % NC_PORT,
            "-device", "virtio-net-device,netdev=n0",
        ]
        for i, path in enumerate(disks):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (path, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
        proc = subprocess.Popen(cmd)
        sock = ac.connect_qemu_socket(SOCK)
        con = ac.Console(sock.fileno(), echo=False)
        pat, bootlog = con.read_until(["AIOS login:"], 120)
        if pat is None:
            raise RuntimeError("no login prompt; tail %r" % bootlog[-500:])
        time.sleep(3)
        con.read_until(["__x__"], 3)
        con.ensure_shell("root", "root", 60, nudge=True, settle=1.0)
        # netconsole auto-starts via getty; give it a beat
        time.sleep(2)

        # ---- phase A: baseline ----
        print("=== A: baseline push (no faults) ===", flush=True)
        s0 = netstat(con)
        check("netstat readable", "tcp_data_segs" in s0, str(s0)[:80])
        nc = NC()
        nc.expect(b"aios# ", 60)
        r = nc.put("/tmp/nrx_a.bin", payload)
        check("A push ok", "__put ok" in r, r[:60])
        got = nc.get("/tmp/nrx_a.bin")
        check("A pull-back identical", got == payload,
              "%d diff runs" % len(diff_runs(got, payload)))
        s1 = netstat(con)
        check("A no checksum drops", s1.get("tcp_cksum_drops", -1) == 0,
              "drops=%s" % s1.get("tcp_cksum_drops"))
        print("  A counters: %s" % {k: s1[k] - s0.get(k, 0)
              for k in s1 if s1[k] != s0.get(k, 0)}, flush=True)

        # ---- phase B: fault injection at increasing brutality ----
        for n, drop in enumerate([50, 10, 3], start=1):
            print("=== B%d: drop every %dth data segment ===" % (n, drop),
                  flush=True)
            sh(con, "cat /proc/netstat.drop.%d" % drop, 30)
            sb0 = netstat(con)
            path = "/tmp/nrx_b%d.bin" % n
            t0 = time.time()
            r = nc.put(path, payload, to=1800)
            dt = time.time() - t0
            check("B%d push ok (%.0fs)" % (n, dt), "__put ok" in r, r[:60])
            sh(con, "cat /proc/netstat.drop.0", 30)   # off for the pull
            got = nc.get(path)
            runs = diff_runs(got, payload)
            detail = ""
            if runs:
                x, y = runs[0]
                detail = "%d runs; first +0x%x len=%d mod1024=%d sample=%s" % (
                    len(runs), x, y - x + 1, x % 1024, got[x:x+16].hex())
                open(os.path.join("/tmp", "netrx_b%d_pulled.bin" % n),
                     "wb").write(got)
            check("B%d stream byte-perfect under loss" % n, not runs, detail)
            sb1 = netstat(con)
            print("  B%d counters: %s" % (n, {k: sb1[k] - sb0.get(k, 0)
                  for k in sb1 if sb1[k] != sb0.get(k, 0)}), flush=True)
            ex = sb1.get("tcp_overlap_trims", 0) + sb1.get("tcp_dup_segs", 0) \
                + sb1.get("tcp_ooo_drops", 0)
            check("B%d retransmit paths exercised" % n,
                  sb1.get("fault_drops", 0) > sb0.get("fault_drops", 0) and ex > 0,
                  "fault_drops=%d reseq-path hits=%d"
                  % (sb1.get("fault_drops", 0) - sb0.get("fault_drops", 0), ex))

        nc.close()

    except Exception as e:
        import traceback
        traceback.print_exc()
        check("harness exception", False, "%s: %s" % (type(e).__name__, e))
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        if os.path.exists(SOCK):
            os.unlink(SOCK)
        shutil.rmtree(workdir, ignore_errors=True)

    npass = sum(1 for _, ok in results if ok)
    print("\n=== netrx QEMU test: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())

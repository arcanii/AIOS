#!/usr/bin/env python3
"""Tight repro loop for the baseline push corruption (QEMU, no fault knob).

Run 1 of netrx_qemu_test.py corrupted the BASELINE push: 1088 in-order TCP
data segments, zero dup/trim/ooo/partial, checksums verified -- and the file
still landed wrong. This loop pushes N times and gathers per-push counter
deltas + full corruption forensics so the failing layer falls out of the
correlation (suspects: parked-reader handoff / split delivery, ring copy,
RECVFROM MR packing, FS_PWRITE path).

Each push goes to a DISTINCT path; corrupt pulls are saved to /tmp.
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
SOCK = "/tmp/aios-netrx2.sock"
NC_PORT = 13232
PUSHES = 8

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)


class NC:
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


def netstat(con):
    out = con.run("cat /proc/netstat", 30)
    stats = {}
    for line in out.splitlines():
        parts = line.strip().split(":")
        if len(parts) == 2 and parts[1].strip().isdigit():
            stats[parts[0].strip()] = int(parts[1].strip())
    return stats


def forensics(tag, got, want, payload):
    diffs = [i for i in range(min(len(got), len(want))) if got[i] != want[i]]
    runs = []
    if diffs:
        st = pv = diffs[0]
        for i in diffs[1:]:
            if i == pv + 1:
                pv = i
            else:
                runs.append((st, pv))
                st = pv = i
        runs.append((st, pv))
    print("  %s: %d corrupt bytes in %d runs (len delta %d)"
          % (tag, len(diffs), len(runs), len(got) - len(want)), flush=True)
    for x, y in runs[:6]:
        L = y - x + 1
        frag = got[x:y+1]
        allff = all(b == 0xFF for b in frag)
        allz = all(b == 0 for b in frag)
        srcs = []
        if not (allff or allz) and L >= 12:
            srcs = [hex(i) for i in range(0, len(payload) - L)
                    if payload[i:i+L] == frag][:3]
        print("    +0x%06x len=%-5d mod512=%-3d mod900=%-3d mod1024=%-4d "
              "mod1448=%-4d allFF=%d allZ=%d" %
              (x, L, x % 512, x % 900, x % 1024, x % 1448, allff, allz),
              flush=True)
        print("      got : %s" % frag[:24].hex(), flush=True)
        print("      want: %s" % want[x:x+24].hex(), flush=True)
        if srcs:
            print("      got-bytes appear in payload at: %s  <-- MIS-SPLICE"
                  % srcs, flush=True)
    return runs


def main():
    ref = open(os.path.join(REPO, "disk/kernel8.img"), "rb").read()
    # 0xFF-FREE payload, same length: any 0xFF in the readback is unambiguous
    # corruption (free space + fresh ext2 blocks are 0x00, so 0xFF must be
    # injected by the rx/write path). Pattern stays position-deterministic.
    payload = bytes((i * 73 + (i >> 7)) & 0x7F for i in range(len(ref)))
    print("payload: %d bytes (0xFF-free)" % len(payload), flush=True)

    workdir = tempfile.mkdtemp(prefix="aios-netrx2-")
    proc = None
    corrupt = 0
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
            raise RuntimeError("no login prompt")
        time.sleep(3)
        con.read_until(["__x__"], 3)
        con.ensure_shell("root", "root", 60, nudge=True, settle=1.0)
        time.sleep(2)

        nc = NC()
        nc.expect(b"aios# ", 60)
        prev = netstat(con)
        for n in range(1, PUSHES + 1):
            path = "/tmp/rp%d.bin" % n
            t0 = time.time()
            r = nc.put(path, payload)
            dt = time.time() - t0
            got = nc.get(path)
            cur = netstat(con)
            delta = {k: cur[k] - prev.get(k, 0)
                     for k in cur if cur[k] != prev.get(k, 0)}
            prev = cur
            ok = got == payload
            print("push %d: %s (%.0fs)  delta=%s"
                  % (n, "CLEAN" if ok else "CORRUPT", dt, delta), flush=True)
            if not ok:
                corrupt += 1
                forensics("push %d" % n, got, payload, payload)
                open("/tmp/netrx_rp%d_pulled.bin" % n, "wb").write(got)
                # Disambiguate write-path vs __get read-path: sha256sum on the
                # Pi reads the file through cat/sha (NOT __get). If it equals
                # the clean payload, the file is fine on disk and __get is
                # corrupting; if it differs, the corruption is on disk (write).
                import hashlib as _h
                want_sha = _h.sha256(payload).hexdigest()
                pi_sha = con.run("sha256sum %s" % path, 120).split()
                pi_sha = next((t for t in pi_sha if len(t) == 64), "?")
                got_sha = _h.sha256(got).hexdigest()
                verdict = ("ON-DISK (write path)" if pi_sha != want_sha
                           else "__get READ path (disk is clean!)")
                print("    pi_sha=%s want=%s pull_sha=%s -> %s"
                      % (pi_sha[:16], want_sha[:16], got_sha[:16], verdict),
                      flush=True)

        final = netstat(con)
        print("\nFINAL netstat dbg: ff_store_off=%d ff_read_off=%d "
              "store_bytes=%d read_bytes=%d handoff=%d split=%d "
              "overlap=%d dup=%d ooo=%d ring_ovf=%d" % (
              final.get("dbg_ff_store_off", -1), final.get("dbg_ff_read_off", -1),
              final.get("dbg_store_bytes", -1), final.get("dbg_read_bytes", -1),
              final.get("tcp_reader_handoff", -1), final.get("tcp_split_deliver", -1),
              final.get("tcp_overlap_trims", -1), final.get("tcp_dup_segs", -1),
              final.get("tcp_ooo_drops", -1), final.get("ring_overflow_drops", -1)),
              flush=True)
        print("\n%d/%d pushes corrupt" % (corrupt, PUSHES), flush=True)

    except Exception:
        import traceback
        traceback.print_exc()
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
    return 0


if __name__ == "__main__":
    sys.exit(main())

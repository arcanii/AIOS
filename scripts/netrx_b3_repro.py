#!/usr/bin/env python3
"""netrx_b3_repro.py -- isolate the netrx_qemu_test B3 failure (drop every 3rd).

B3 deterministically dies with a client-side ConnectionResetError mid-__put.
The AIOS stack never sends RST, so the reset must come from SLIRP abandoning
the guest-side connection. This repro watches the death happen: it polls
/proc/netstat over the SERIAL console every 8s during the lossy push and
prints a timeline of dbg_store_bytes (received payload progress) vs
fault_drops/dup/ooo counters, then classifies:

  PHASE-LOCK   store_bytes frozen while fault_drops climbs (same segment
               eaten repeatedly -> SLIRP rexmit backoff -> abort)
  BACKOFF-ABORT progress crawls, gaps grow (RTO ladder), SLIRP gives up
  PASS         the put completes (B3 is flaky, not deterministic)

Usage: python3 scripts/netrx_b3_repro.py [--drop 3] [--cap 600]
Per qemu-test-hygiene: private disk copies, unique socket + host port.
"""
import argparse
import hashlib
import importlib.util
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-netrx-b3-%d.sock" % os.getpid()
NC_PORT = 13251 + (os.getpid() % 40)

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)


def netstat(con):
    out = con.run("cat /proc/netstat", 30)
    stats = {}
    for line in out.splitlines():
        parts = line.strip().split(":")
        if len(parts) == 2 and parts[1].strip().isdigit():
            stats[parts[0].strip()] = int(parts[1].strip())
    return stats


class PutThread(threading.Thread):
    def __init__(self, port, payload):
        super().__init__(daemon=True)
        self.port = port
        self.payload = payload
        self.outcome = None      # "ok" / "reset" / "timeout" / other error str
        self.t_end = None

    def run(self):
        try:
            s = socket.create_connection(("127.0.0.1", self.port), timeout=20)
            s.settimeout(60)
            buf = b""
            dl = time.time() + 60
            while b"aios# " not in buf and time.time() < dl:
                try:
                    buf += s.recv(65536)
                except socket.timeout:
                    pass
            s.sendall(("__put /tmp/b3.bin %d\n" % len(self.payload)).encode())
            s.sendall(self.payload)
            buf = b""
            dl = time.time() + 1800
            while time.time() < dl:
                try:
                    d = s.recv(65536)
                    if d:
                        buf += d
                        if b"__put ok" in buf:
                            self.outcome = "ok"
                            break
                except socket.timeout:
                    continue
                except ConnectionResetError:
                    self.outcome = "reset"
                    break
                except OSError as e:
                    self.outcome = "oserror: %s" % e
                    break
            if self.outcome is None:
                self.outcome = "timeout"
        except Exception as e:  # noqa: BLE001 -- report any client death verbatim
            self.outcome = "client-error: %s" % e
        self.t_end = time.time()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--drop", type=int, default=3)
    ap.add_argument("--cap", type=int, default=600, help="watch cap seconds")
    ap.add_argument("--pcap", default=None,
                    help="dump guest link traffic to this pcap (filter-dump)")
    a = ap.parse_args()

    payload = open(os.path.join(REPO, "disk/kernel8.img"), "rb").read()
    print("payload %d bytes  drop.%d  port %d" % (len(payload), a.drop, NC_PORT),
          flush=True)

    workdir = tempfile.mkdtemp(prefix="aios-b3-")
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
        if a.pcap:
            cmd += ["-object", "filter-dump,id=fd0,netdev=n0,file=%s" % a.pcap]
        for i, path in enumerate(disks):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (path, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
        proc = subprocess.Popen(cmd)
        sock = ac.connect_qemu_socket(SOCK)
        con = ac.Console(sock.fileno(), echo=False)
        pat, bootlog = con.read_until(["AIOS login:"], 120)
        if pat is None:
            raise RuntimeError("no login prompt; tail %r" % bootlog[-400:])
        time.sleep(3)
        con.read_until(["__x__"], 3)
        con.ensure_shell("root", "root", 60, nudge=True, settle=1.0)
        time.sleep(2)

        con.run("cat /proc/netstat.drop.%d" % a.drop, 30)
        s0 = netstat(con)
        base = {k: s0.get(k, 0) for k in
                ["dbg_store_bytes", "fault_drops", "tcp_data_segs",
                 "tcp_dup_segs", "tcp_ooo_drops", "tcp_read_acks",
                 "tcp_overlap_trims"]}

        put = PutThread(NC_PORT, payload)
        t0 = time.time()
        put.start()

        print("%6s %9s %7s %8s %7s %7s %8s %7s" % (
            "t", "stored", "drops", "datasegs", "dups", "ooo", "readacks",
            "trims"), flush=True)
        last_stored = -1
        frozen = 0
        while put.outcome is None and time.time() - t0 < a.cap:
            time.sleep(8)
            s = netstat(con)
            row = {k: s.get(k, 0) - base[k] for k in base}
            stored = row["dbg_store_bytes"]
            print("%6.0f %9d %7d %8d %7d %7d %8d %7d" % (
                time.time() - t0, stored, row["fault_drops"],
                row["tcp_data_segs"], row["tcp_dup_segs"],
                row["tcp_ooo_drops"], row["tcp_read_acks"],
                row["tcp_overlap_trims"]), flush=True)
            frozen = frozen + 1 if stored == last_stored else 0
            last_stored = stored

        put.join(5)
        dt = (put.t_end or time.time()) - t0
        print("---", flush=True)
        print("outcome: %s after %.0fs  stored=%d/%d  frozen-polls=%d" % (
            put.outcome, dt, last_stored, len(payload), frozen), flush=True)
        if put.outcome == "ok":
            print("CLASSIFY: PASS (B3 not deterministic here)")
        elif frozen >= 3:
            print("CLASSIFY: PHASE-LOCK then %s (progress frozen %d polls "
                  "before death)" % (put.outcome, frozen))
        else:
            print("CLASSIFY: %s with crawling progress (backoff-abort likely)"
                  % put.outcome)
    finally:
        if proc:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(workdir, ignore_errors=True)
        if os.path.exists(SOCK):
            os.unlink(SOCK)


if __name__ == "__main__":
    main()

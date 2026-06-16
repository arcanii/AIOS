#!/usr/bin/env python3
"""pipe_throughput_qemu.py -- single-stream pipe throughput probe (coalescing A/B).

Times `seq 1 N | wc -l` over netconsole on QEMU smp=4. `seq` writes ~N*6 bytes into the
pipe; that write path is the coalescing target (MR 900B vs SHM 4KB). wc's read side already
uses SHM. Reports the wall time of the pipeline (median of a few runs) -- compare a baseline
kernel vs a coalesced kernel. Usage: python3 scripts/pipe_throughput_qemu.py [N] [runs]
"""
import os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get("AIOS_KERNEL", os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
PORT = 47100 + (os.getpid() % 1500)
N = int(sys.argv[1]) if len(sys.argv) > 1 else 50000
RUNS = int(sys.argv[2]) if len(sys.argv) > 2 else 3
PROMPT = b"aios#"

def ru(s, marker, wait):
    t0 = time.time(); buf = b""; s.settimeout(1.0)
    while time.time() - t0 < wait:
        try: x = s.recv(4096)
        except socket.timeout:
            if marker in buf: break
            continue
        if not x: break
        buf += x
        if marker in buf: break
    return buf

def wait_up(port, dl_s=150):
    dl = time.time() + dl_s
    while time.time() < dl:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=8)
            if b"aios#" in ru(s, PROMPT, 8): s.close(); return True
            s.close()
        except (OSError, socket.timeout): pass
        time.sleep(1.0)
    return False

def timed(port, cmd, wait):
    s = socket.create_connection(("127.0.0.1", port), timeout=12); ru(s, PROMPT, 10)
    t0 = time.time(); s.sendall((cmd + "\n").encode()); out = ru(s, PROMPT, wait)
    dt = time.time() - t0; s.close()
    return dt, out.decode("utf-8", "replace")

def main():
    if not os.path.exists(KERNEL): print("FAIL: kernel missing"); return 2
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on", "-cpu", "cortex-a53",
           "-smp", "4", "-m", "2G", "-display", "none", "-monitor", "none", "-no-reboot",
           "-serial", "file:/tmp/pt_%d.log" % os.getpid(),
           "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK, "-device", "virtio-blk-device,drive=hd0"]
    if os.path.exists(LOGDISK):
        cmd += ["-drive", "file=%s,format=raw,if=none,id=hd1" % LOGDISK, "-device", "virtio-blk-device,drive=hd1"]
    cmd += ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:2323" % PORT,
            "-device", "virtio-net-device,netdev=n0", "-kernel", KERNEL]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    times = []
    try:
        if not wait_up(PORT): print("FAIL: netconsole never up"); return 2
        timed(PORT, "echo warm", 8)
        probe = "seq 1 %d | wc -l" % N        # direct (2 fork levels), count in the output
        for r in range(RUNS):
            dt, out = timed(PORT, probe, 90)
            ok = str(N) in out and "Cannot fork" not in out
            print("  run %d: %.2fs  %s" % (r, dt, "ok" if ok else "BAD(%r)" % out.strip()[-50:]), flush=True)
            if ok: times.append(dt)
    finally:
        p.terminate()
        try: p.wait(5)
        except Exception: p.kill()
        try: os.remove("/tmp/pt_%d.log" % os.getpid())
        except OSError: pass
    if times:
        times.sort()
        print("=== seq 1 %d | wc -l : median=%.2fs (min=%.2f) over %d ok runs ==="
              % (N, times[len(times)//2], times[0], len(times)))
    else:
        print("=== no clean runs ===")
    return 0 if times else 1

if __name__ == "__main__":
    sys.exit(main())

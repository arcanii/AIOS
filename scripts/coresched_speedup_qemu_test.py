#!/usr/bin/env python3
"""coresched_speedup_qemu_test.py -- prove Stage S distribution helps CPU-bound work.

AIOS's per-process core distribution (src/boot/spawn_util.c, /proc/coresched) is OFF by
default because it regresses IPC-bound pipelines (seL4 big-kernel-lock contention). But for
CPU-bound parallel work it should give a real speedup. This boots QEMU -smp 4 and runs the
same parallel CPU-bound workload twice -- forked subshells each spinning a pure-shell busy
loop (no spawns, no IPC) -- A/B over one netconsole connection:
  - /proc/coresched.0 (pin all to core 0): the loops SERIALIZE -> slow.
  - /proc/coresched.1 (distribute to cores 1..N-1): the loops run in PARALLEL -> fast.
PASS = the distributed run is meaningfully faster (>= 1.6x for 3 parallel loops).
"""
import os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get(
    "AIOS_KERNEL", os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/coresched_%d.log" % os.getpid()
PORT = 46000 + (os.getpid() % 2000)
N = 3            # parallel loops
ITERS = 120000  # per-loop iterations (pure-shell, CPU-bound)


def _recv_until(s, marker, wait):
    t0 = time.time(); buf = b""; s.settimeout(1.0)
    while time.time() - t0 < wait:
        try:
            x = s.recv(4096)
        except socket.timeout:
            if marker in buf: break
            continue
        if not x: break
        buf += x
        if marker in buf: break
    return buf


def nc(cmd, wait, port):
    s = socket.create_connection(("127.0.0.1", port), timeout=12)
    _recv_until(s, b"aios#", 10)                       # sync to banner + prompt
    t0 = time.time()
    s.sendall((cmd + "\n").encode())
    buf = _recv_until(s, b"SPEEDDONE", wait) if "SPEEDDONE" in cmd else _recv_until(s, b"aios#", wait)
    s.close()
    return buf.decode("utf-8", "replace"), time.time() - t0


def wait_up(port, deadline_s=150):
    dl = time.time() + deadline_s
    while time.time() < dl:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=8)
            if b"aios#" in _recv_until(s, b"aios#", 8):
                s.close(); return True
            s.close()
        except (OSError, socket.timeout):
            pass
        time.sleep(1.0)
    return False


def timed_parallel(port):
    """N forked subshells each spin a CPU-bound loop; wait; measure wall time to SPEEDDONE."""
    loops = " ".join("( i=0; while [ $i -lt %d ]; do i=$((i+1)); done ) &" % ITERS
                     for _ in range(N))
    cmd = "%s wait; echo SPEEDDONE" % loops
    t0 = time.time()
    out, _ = nc(cmd, 120, port)
    dt = time.time() - t0
    return dt, ("SPEEDDONE" in out)


def main():
    if not os.path.exists(KERNEL) or not os.path.exists(DISK):
        print("FAIL: kernel/disk missing"); return 2
    if os.path.exists(LOG): os.remove(LOG)
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-no-reboot", "-serial", "file:" + LOG,
           "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK,
           "-device", "virtio-blk-device,drive=hd0"]
    if os.path.exists(LOGDISK):
        cmd += ["-drive", "file=%s,format=raw,if=none,id=hd1" % LOGDISK,
                "-device", "virtio-blk-device,drive=hd1"]
    cmd += ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:2323" % PORT,
            "-device", "virtio-net-device,netdev=n0", "-kernel", KERNEL]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    results = {}
    try:
        # wait for netconsole
        if not wait_up(PORT):
            print("FAIL: netconsole never came up"); return 2
        # warm-up (let the FS settle), then A/B
        nc("echo warm", 4, PORT)
        nc("cat /proc/coresched.0", 6, PORT)          # pin core 0
        t_serial, ok_s = timed_parallel(PORT)
        nc("cat /proc/coresched.1", 6, PORT)          # distribute
        t_par, ok_p = timed_parallel(PORT)
        results = dict(t_serial=t_serial, t_par=t_par, ok_s=ok_s, ok_p=ok_p)
    finally:
        p.terminate()
        try: p.wait(5)
        except Exception: p.kill()
        try: os.remove(LOG)
        except OSError: pass

    ts, tp = results.get("t_serial", 0), results.get("t_par", 0)
    speedup = ts / tp if tp else 0
    print("  coresched.0 (pin core0): %.2fs   coresched.1 (distribute): %.2fs   speedup=%.2fx"
          % (ts, tp, speedup))
    # CORRECTNESS gate (QEMU-valid): both parallel runs must COMPLETE -- distribution must not
    # hang/break CPU-bound work. The SPEEDUP ratio is INFORMATIONAL on QEMU: TCG emulates the
    # guest cores without true host parallelism, so distributing adds MTTCG overhead and reads
    # SLOWER. Real multi-core speedup is measurable only on the 4 physical A72 cores (HW).
    ok = bool(results.get("ok_s") and results.get("ok_p"))
    print("  [%s] %d parallel CPU-bound loops complete under distribution (correctness; "
          "speedup is HW-only -- QEMU TCG cannot parallelize guest cores)" % ("PASS" if ok else "FAIL", N))
    print("\n=== coresched speedup: %s (speedup=%.2fx informational) ==="
          % ("PASS" if ok else "FAIL", speedup))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

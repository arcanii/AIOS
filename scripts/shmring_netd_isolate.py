#!/usr/bin/env python3
# Isolation: does the DIRECT SHM-ring engage on the netd-ON build in QEMU?
# HW (build-rpi4-netd / netd-ON / A72) showed map_ok=0; QEMU build-04 (netd-OFF)
# showed map_ok=33 with the SAME disk binaries. This boots build-netd (netd-ON,
# QEMU-virt) over the SERIAL console (netconsole is flaky on build-netd) and checks
# map_ok. map_ok==0 here => netd-ON is the cause (local repro!); map_ok>0 => the
# A72 silicon is the variable.
import os, importlib.util, shutil, subprocess, sys, tempfile, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get("AIOS_ISO_KERNEL",
    os.path.join(REPO, "build-netd/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
spec = importlib.util.spec_from_file_location("aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec); spec.loader.exec_module(ac)
SOCK = "/tmp/aios-shmnetd-%d.sock" % os.getpid()

def qemu_cmd(disk, logdisk):
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-monitor", "none", "-no-reboot",
           "-serial", "unix:%s,server" % SOCK, "-kernel", KERNEL]
    for i, p in enumerate([disk, logdisk]):
        if p and os.path.exists(p):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (p, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    cmd += ["-netdev", "user,id=n0", "-device", "virtio-net-device,netdev=n0"]
    return cmd

def mapok(s):
    for t in s.split():
        if t.startswith("map_ok="): return int(t.split("=")[1])
    return -1

def main():
    if not os.path.exists(KERNEL):
        print("FAIL: %s missing (ninja -C build-netd)" % KERNEL); return 2
    tmp = tempfile.mkdtemp(prefix="aios-shmnetd-")
    disk = os.path.join(tmp, "disk.img"); shutil.copy(DISK, disk)
    logdisk = os.path.join(tmp, "log.img")
    if os.path.exists(LOGDISK): shutil.copy(LOGDISK, logdisk)
    else: logdisk = None
    if os.path.exists(SOCK): os.unlink(SOCK)
    proc = None; sock = None
    logf = open(os.path.join(tmp, "serial.log"), "w")
    try:
        proc = subprocess.Popen(qemu_cmd(disk, logdisk))
        sock = ac.connect_qemu_socket(SOCK, timeout=20)
        sess = ac.Console(sock.fileno(), logfile=logf, echo=False)
        print("=== boot + login (build-netd, netd-ON, serial) ===", flush=True)
        sess.read_until("\x00settle\x00", 30)
        ok = False
        for _ in range(3):
            sess.buf = ""
            try:
                sess.ensure_shell("root", "root", 90, nudge=True, settle=1.0); ok = True; break
            except (TimeoutError, RuntimeError): time.sleep(3)
        if not ok:
            print("FAIL: serial login"); return 1
        print("logged in.", flush=True)
        print("ver  :", sess.run("cat /proc/version", 15).strip(), flush=True)
        print("arm  :", sess.run("cat /proc/shmring.1", 15).strip(), flush=True)
        for n in (10, 100, 1000, 100000):
            rr = sess.run("seq 1 %d | wc -l" % n, 45)
            print("pipe : seq 1 %d | wc -l -> %r  (want %d)" % (n, rr.strip(), n), flush=True)
        r = "100000"
        m = sess.run("cat /proc/shmring", 15).strip()
        print("ctrs :", m, flush=True)
        sess.run("cat /proc/shmring.0", 10)
        mk = mapok(m)
        print("\n=== ISOLATION VERDICT (build-netd / netd-ON / QEMU) ===")
        print("data seq|wc -l =", r, "(want 100000):", "OK" if r == "100000" else "MISMATCH")
        print("map_ok =", mk)
        if mk > 0:
            print(">>> DIRECT RING ENGAGES on netd-ON QEMU -> the HW failure is the A72 SILICON, not netd.")
        elif mk == 0:
            print(">>> map_ok=0 on netd-ON QEMU -> NETD-ON is the cause (LOCAL REPRO). Debug pipe_map_ring here.")
        else:
            print(">>> could not read map_ok")
        return 0
    except Exception as e:
        print("EXCEPTION:", repr(e)); return 1
    finally:
        try: logf.close()
        except OSError: pass
        if sock is not None:
            try: sock.close()
            except OSError: pass
        if proc is not None:
            proc.terminate()
            try: proc.wait(timeout=10)
            except subprocess.TimeoutExpired: proc.kill()
        if os.path.exists(SOCK): os.unlink(SOCK)
        print("serial log:", os.path.join(tmp, "serial.log"))

if __name__ == "__main__":
    sys.exit(main())

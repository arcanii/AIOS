#!/usr/bin/env python3
"""Phase A step 3 confinement-gate worker -- QEMU MECHANISM smoke test.

The DECISIVE experiment (does work on a secondary survive a peer wedge?) needs real HW (the
BCM2711 fabric stall + the mini-UART out-of-band report). QEMU has neither, so this only
verifies the worker MECHANISM is sound before flashing:
  * /proc/confine.N arms a syscall-doing worker pinned to core N; .r disarms
  * while armed the worker tick ADVANCES (the thread runs + does seL4_Yield syscalls on core N)
  * while disarmed the tick is FLAT
  * the system stays healthy

On HW: arm it on a secondary (e.g. /proc/confine.2), force a teardown-after-idle wedge, and
read sercap -- the [WDOG] line reports "worker(core 2) advanced=K during the wedge". K>0 =>
the secondary's syscalls were NOT BKL-blocked => confinement holds => Phase B viable.

Hygiene: private disk copies, unique port + serial log, QEMU via its own Popen handle.
"""
import os, socket, subprocess, sys, time, shutil

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get(
    "AIOS_KERNEL",
    os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
SRC_DISK = os.path.join(REPO, "disk/disk_ext2.img")
SRC_LOG = os.path.join(REPO, "disk/log_ext2.img")

PID = os.getpid()
PORT = 2400 + (PID % 150)
SERIAL_LOG = "/tmp/aios-confine-%d.log" % PID
PRIV_DISK = "/tmp/aios-confine-disk-%d.img" % PID
PRIV_LOG = "/tmp/aios-confine-log-%d.img" % PID


def qemu_cmd():
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-monitor", "none", "-no-reboot",
           "-serial", "file:%s" % SERIAL_LOG, "-kernel", KERNEL]
    for i, path in enumerate([PRIV_DISK, PRIV_LOG]):
        if os.path.exists(path):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (path, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    cmd += ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:2323" % PORT,
            "-device", "virtio-net-device,netdev=n0"]
    return cmd


class NC:
    def __init__(self, port=PORT, connect_to=10):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=connect_to)
        self.s.settimeout(1.0); self.buf = b""

    def expect(self, pat=b"aios# ", to=30):
        pat = pat.encode() if isinstance(pat, str) else pat
        dl = time.time() + to
        while True:
            i = self.buf.find(pat)
            if i != -1:
                out = self.buf[:i]; self.buf = self.buf[i + len(pat):]
                return out.decode("utf-8", "replace")
            if time.time() > dl:
                raise TimeoutError("expect %r timeout; buf=%r" % (pat, self.buf[-200:]))
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            if not d:
                raise EOFError("closed; buf=%r" % self.buf[-200:])
            self.buf += d

    def cmd(self, line, to=30):
        self.s.sendall((line + "\n").encode())
        return self.expect(b"aios# ", to)

    def close(self):
        try:
            self.s.sendall(b"exit\n"); self.s.close()
        except OSError:
            pass


def wait_netconsole(deadline_s=150):
    dl = time.time() + deadline_s
    while time.time() < dl:
        try:
            nc = NC(); nc.expect(b"aios# ", to=8); return nc
        except (OSError, TimeoutError, EOFError):
            time.sleep(1.0)
    raise TimeoutError("netconsole never came up within %ds" % deadline_s)


def ticks(out):
    # parse "... ticks=NNNN ..."
    for tok in out.replace("\n", " ").split():
        if tok.startswith("ticks="):
            try: return int(tok.split("=", 1)[1])
            except ValueError: return None
    return None


def main():
    print("=== confinement-gate worker MECHANISM smoke test (port %d) ===" % PORT, flush=True)
    shutil.copy(SRC_DISK, PRIV_DISK)
    if os.path.exists(SRC_LOG): shutil.copy(SRC_LOG, PRIV_LOG)
    open(SERIAL_LOG, "w").close()
    proc = subprocess.Popen(qemu_cmd())
    nc = None
    verdicts = []
    def vcheck(name, ok, detail=""):
        verdicts.append(bool(ok))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  -- " + detail) if detail else ""), flush=True)

    try:
        print("=== booting QEMU -smp 4 ===", flush=True)
        nc = wait_netconsole()
        print("=== netconsole up ===", flush=True)

        d0 = nc.cmd("cat /proc/confine")
        print("  " + d0.strip().splitlines()[0], flush=True)
        vcheck("(1) /proc/confine present, disarmed by default", "armed=0" in d0)

        # arm on core 2, confirm it advances
        a = nc.cmd("cat /proc/confine.2")
        vcheck("(2) /proc/confine.2 arms worker on core 2", "armed=1 core=2" in a, a.strip()[:60])
        t1 = ticks(nc.cmd("cat /proc/confine"))
        time.sleep(2.0)
        t2 = ticks(nc.cmd("cat /proc/confine"))
        vcheck("(3) armed worker ADVANCES (syscalls run on core 2)",
               t1 is not None and t2 is not None and t2 > t1, "ticks %s -> %s" % (t1, t2))

        # disarm, confirm it stops
        nc.cmd("cat /proc/confine.r")
        t3 = ticks(nc.cmd("cat /proc/confine"))
        time.sleep(2.0)
        t4 = ticks(nc.cmd("cat /proc/confine"))
        vcheck("(4) disarmed worker is FLAT", t3 is not None and t4 == t3,
               "ticks %s -> %s" % (t3, t4))

        vcheck("(5) healthy after arm/disarm", "ALIVE" in nc.cmd("echo ALIVE", to=20))
    except Exception as e:
        vcheck("harness exception: %s: %s" % (type(e).__name__, e), False)
    finally:
        try:
            if nc: nc.close()
        except Exception: pass
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()
        for f in (PRIV_DISK, PRIV_LOG):
            try: os.remove(f)
            except OSError: pass

    npass = sum(1 for ok in verdicts if ok)
    print("\n=== confine worker mechanism: %d/%d checks passed ===" % (npass, len(verdicts)), flush=True)
    return 0 if verdicts and npass == len(verdicts) else 1


if __name__ == "__main__":
    sys.exit(main())

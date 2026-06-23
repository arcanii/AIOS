#!/usr/bin/env python3
"""General work assignment -- QEMU smoke test of the PLUGGABLE placement policy
(/proc/placement). The strategy LOGIC is verified deterministically by the host test
(scripts/placement_strategy_host_test.c, 16/16); this confirms the kernel WIRING:

  * the strategy menu is exposed and selectable (.sN), threshold settable (.tN)
  * with the policy ON, new user procs route OFF the over-threshold server core 0 onto the
    cold cores (a before/after delta -- tolerant, since fork+exec + a transient `cat` reader
    + the timer-mask contention on secondaries make absolute counts noisy)
  * /proc/pincore overrides the policy

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
PORT = 2400 + (PID % 160)
SERIAL_LOG = "/tmp/aios-place2-%d.log" % PID
PRIV_DISK = "/tmp/aios-place2-disk-%d.img" % PID
PRIV_LOG = "/tmp/aios-place2-log-%d.img" % PID


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


def parse_load(out):
    m = {}
    for ln in out.split("\n"):
        p = ln.split()
        if len(p) >= 6 and p[0] == "core" and p[2] == "load" and p[4] == "(servers":
            try:
                m[int(p[1])] = (int(p[3]), int(p[5].rstrip(")")))
            except ValueError:
                pass
    return m


def procs(out):
    return {c: ld - sv for c, (ld, sv) in parse_load(out).items()}


def main():
    print("=== general work assignment: pluggable placement policy (port %d) ===" % PORT, flush=True)
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

        # 1. default + strategy menu exposed
        base = nc.cmd("cat /proc/placement")
        print("  " + base.splitlines()[0], flush=True)
        vcheck("(1) default auto=0 strategy=hybrid; servers on core 0",
               "auto=0" in base and "strategy=0(hybrid)" in base
               and parse_load(base).get(0, (0, 0))[1] >= 8)
        menu = next((l.strip() for l in base.splitlines() if "strategies:" in l), "")
        vcheck("(2) strategy menu lists hybrid/spread/pack/rr",
               all(s in base for s in ("0=hybrid", "1=spread", "2=pack", "3=rr")), menu)

        # 3. strategy selection + threshold are settable and reported
        s1 = nc.cmd("cat /proc/placement.s1")
        s2 = nc.cmd("cat /proc/placement.s2")
        s9 = nc.cmd("cat /proc/placement.s9")   # out of range -> ignored, stays 2
        t3 = nc.cmd("cat /proc/placement.t3")
        vcheck("(3) .sN selects strategy, .s9 ignored, .tN sets threshold",
               "strategy=1(spread)" in s1 and "strategy=2(pack)" in s2
               and "strategy=2(pack)" in s9 and "spill_threshold=3" in t3,
               "s1=%s s9=%s t3=%s" % ("spread" in s1, "pack" in s9, "thr3" in t3))

        # back to hybrid, threshold 2, policy ON
        nc.cmd("cat /proc/placement.s0"); nc.cmd("cat /proc/placement.t2")
        before = procs(nc.cmd("cat /proc/placement"))
        nc.cmd("cat /proc/placement.1")

        # 4. WIRING: a proc spawned under the hybrid policy routes OFF the loaded core 0
        nc.cmd("sleep 60 &", to=60); time.sleep(3.0)
        after = procs(nc.cmd("cat /proc/placement", to=60))
        d0 = after.get(0, 0) - before.get(0, 0)
        dcold = sum(after.get(c, 0) - before.get(c, 0) for c in (1, 2, 3))
        print("  proc delta: core0 %+d, cold cores %+d" % (d0, dcold), flush=True)
        vcheck("(4) policy routes new procs OFF the loaded core 0 onto cold cores",
               d0 <= 0 and dcold >= 1, "core0 delta=%d, cold delta=%d" % (d0, dcold))

        # 5. pincore override beats the policy
        nc.cmd("cat /proc/pincore.0")
        b0 = procs(nc.cmd("cat /proc/placement", to=60)).get(0, 0)
        nc.cmd("sleep 60 &", to=60); time.sleep(2.5)
        a0 = procs(nc.cmd("cat /proc/placement", to=60)).get(0, 0)
        vcheck("(5) pincore.0 overrides policy (proc lands on core 0)", a0 == b0 + 1,
               "core0 procs %d -> %d" % (b0, a0))
        nc.cmd("cat /proc/pincore.r"); nc.cmd("cat /proc/placement.0")

        vcheck("(6) healthy after policy exercise", "ALIVE" in nc.cmd("echo ALIVE", to=30))
        print("\n--- final /proc/placement ---", flush=True)
        print(nc.cmd("cat /proc/placement").strip(), flush=True)
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
    print("\n=== placement policy wiring: %d/%d checks passed ===" % (npass, len(verdicts)), flush=True)
    return 0 if verdicts and npass == len(verdicts) else 1


if __name__ == "__main__":
    sys.exit(main())

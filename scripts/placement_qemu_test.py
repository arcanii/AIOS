#!/usr/bin/env python3
"""Phase A step-3 prerequisites -- QEMU verification of the two core-placement primitives:

  /proc/distribute[.0|.1|.2]  -- server distribution MODE; mode 2 = SURGICAL (pipe->1,
      exec->2, fs->3, everything else home on core 0). /proc/distribute also LISTS each
      server's assigned core, so placement is directly checkable.
  /proc/pincore[.N|.r]        -- bind every subsequent user spawn to core N (overrides
      coresched), .r resets. The deterministic per-core SESSION bind the HW confinement
      gate needs (a victim on core N, a survivor on core M).

Checks (single boot; placement is read-only /proc, correctness uses the smp gate's clean
`seq|wc &` token pattern -- no `case "$(...)"` leak, no multi-session relay limit):
  1. mode 0 (off)      -> every server core 0
  2. mode 2 (surgical) -> pipe=1, exec=2, fs=3; net/flush/serverstats/display/root = 0
  3. mode 1 (full)     -> cores spread across {0,1,2,3}
  4. pincore knob      -> default -1; .2 -> 2; .r -> -1; .9 (out of range) ignored
  5. correctness under surgical (mode 2): fork storm still computes 500 (no corruption)
  6. pincore behavioral: with coresched+pincore.1, a forked command still computes correctly

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
PORT = 2400 + (PID % 170)
SERIAL_LOG = "/tmp/aios-place-%d.log" % PID
PRIV_DISK = "/tmp/aios-place-disk-%d.img" % PID
PRIV_LOG = "/tmp/aios-place-log-%d.img" % PID
BIG_TO = 90


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


def parse_placement(out):
    """Parse the /proc/distribute per-server listing into {name: core}."""
    m = {}
    for ln in out.split("\n"):
        p = ln.split()
        # lines look like:  "pipe          core 1"
        if len(p) >= 3 and p[-2] == "core" and p[-1].isdigit():
            m[p[0]] = int(p[-1])
    return m


def main():
    print("=== Phase A step-3 primitives: /proc/distribute (surgical) + /proc/pincore (port %d) ===" % PORT, flush=True)
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

        # 1. mode 0 (off): every server on core 0
        off = parse_placement(nc.cmd("cat /proc/distribute.0"))
        vcheck("(1) mode 0 off: all servers core 0",
               bool(off) and all(c == 0 for c in off.values()),
               "n=%d %s" % (len(off), off))

        # 2. mode 2 (surgical): pipe=1 exec=2 fs=3; the rest home on 0
        surg = parse_placement(nc.cmd("cat /proc/distribute.2"))
        ok_surg = (surg.get("pipe") == 1 and surg.get("exec") == 2 and surg.get("fs") == 3
                   and surg.get("net", 0) == 0 and surg.get("flush", 0) == 0
                   and surg.get("serverstats", 0) == 0 and surg.get("root", 0) == 0)
        vcheck("(2) mode 2 surgical: pipe=1 exec=2 fs=3, rest home", ok_surg,
               "pipe=%s exec=%s fs=%s net=%s flush=%s root=%s"
               % (surg.get("pipe"), surg.get("exec"), surg.get("fs"),
                  surg.get("net"), surg.get("flush"), surg.get("root")))

        # 3. mode 1 (full round-robin): cores spread across all 4
        full = parse_placement(nc.cmd("cat /proc/distribute.1"))
        cores_used = set(full.values())
        vcheck("(3) mode 1 full: cores spread across {0,1,2,3}",
               cores_used == {0, 1, 2, 3}, "cores_used=%s" % sorted(cores_used))

        # back to off for the rest
        nc.cmd("cat /proc/distribute.0")

        # 4. pincore knob states
        p_def = nc.cmd("cat /proc/pincore")
        p_set = nc.cmd("cat /proc/pincore.2")
        p_oor = nc.cmd("cat /proc/pincore.9")   # out of range -> ignored, stays 2
        p_rst = nc.cmd("cat /proc/pincore.r")
        vcheck("(4) pincore knob: default -1, .2->2, .9 ignored, .r->-1",
               "target=-1" in p_def and "target=2" in p_set
               and "target=2" in p_oor and "target=-1" in p_rst,
               "def=%r set=%r oor=%r rst=%r"
               % (p_def.strip()[-12:], p_set.strip()[-12:], p_oor.strip()[-12:], p_rst.strip()[-12:]))

        # 5. correctness under surgical (mode 2): fork storm still computes 500
        nc.cmd("cat /proc/distribute.2")
        bad = 0
        for r in range(5):
            out = nc.cmd("seq 1 500 | wc -l & seq 1 500 | wc -l & seq 1 500 | wc -l & wait", to=BIG_TO)
            if out.count("500") != 3:
                bad += 1
            time.sleep(0.3)
        vcheck("(5) surgical mode: fork storm correct (no corruption)", bad == 0,
               "%d/5 rounds clean" % (5 - bad))
        nc.cmd("cat /proc/distribute.0")

        # 6. pincore behavioral: coresched on + pincore.1, a forked command still computes right
        nc.cmd("cat /proc/coresched.1")
        nc.cmd("cat /proc/pincore.1")
        r6 = nc.cmd("seq 1 100 | wc -l", to=BIG_TO).strip()
        vcheck("(6) pincore.1 + coresched: forked command still correct (=100)",
               "100" in r6, "got=%r" % r6[-20:])
        nc.cmd("cat /proc/pincore.r"); nc.cmd("cat /proc/coresched.0")

        # liveness
        vcheck("system healthy after primitive exercise", "ALIVE" in nc.cmd("echo ALIVE", to=30))

        print("\n--- surgical placement listing (mode 2) ---", flush=True)
        print(nc.cmd("cat /proc/distribute.2").strip(), flush=True)
        nc.cmd("cat /proc/distribute.0")
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
    print("\n=== placement primitives: %d/%d checks passed ===" % (npass, len(verdicts)), flush=True)
    return 0 if verdicts and npass == len(verdicts) else 1


if __name__ == "__main__":
    sys.exit(main())

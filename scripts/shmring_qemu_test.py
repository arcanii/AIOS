#!/usr/bin/env python3
"""shmring_qemu_test.py -- SHM-ring pipe LOGIC + data-exactness on QEMU -smp 4.

The direct SPSC SHM-ring (/proc/shmring, v0.4.258) moves pipe bytes in userspace so
a `cmd | cmd` pipeline can span cores. QEMU CANNOT validate the A72 cross-core
coherency (TCG serialises guest cores) -- that needs real hardware. What QEMU CAN
validate is that the ring LOGIC is correct and loses/duplicates no bytes: the block/
wake handshake, EOF, teardown, the server-mediated fallback, and the kill switch.

Every check is DETERMINISTIC (exact byte/line counts cross-checked against a
non-pipe reference), so a ring bug shows up as a wrong number or a hang. Driven over
the auto-started netconsole (TCP 2323) like smp_qemu_test. No `>` redirects (they
break the relay); outputs kept short/atomic so concurrent writers do not interleave.

Run with shmring OFF (legacy, the default) AND ON, AND ON+coresched.1 (distribute) --
the logic must hold identically in all three. Usage: python3 scripts/shmring_qemu_test.py
"""
import os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get(
    "AIOS_KERNEL",
    os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SERIAL_LOG = "/tmp/shmring_boot_serial.log"
PORT = 47200 + (os.getpid() % 1200)

results = []
def check(name, ok, detail=""):
    results.append(ok)
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  -- " + detail) if detail else ""))


def qemu_cmd():
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-monitor", "none", "-no-reboot",
           "-serial", "file:%s" % SERIAL_LOG, "-kernel", KERNEL]
    for i, path in enumerate([DISK, LOGDISK]):
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

    def run(self, cmd, to=30):
        """Send one command, return its stdout (between the echo and the next prompt)."""
        self.s.sendall((cmd + "\n").encode())
        out = self.expect(b"aios# ", to)
        # strip the echoed command line if present
        nl = out.find("\n")
        if nl != -1 and cmd in out[:nl + len(cmd) + 2]:
            out = out[nl + 1:]
        return out.strip()

    def close(self):
        try: self.s.close()
        except Exception: pass


def wait_netconsole(dl_s=150):
    dl = time.time() + dl_s
    while time.time() < dl:
        try:
            nc = NC()
            nc.expect(b"aios# ", to=8)
            return nc
        except (OSError, socket.timeout, EOFError, TimeoutError):
            time.sleep(1.0)
    return None


def num(s):
    """Extract the first integer token from s (e.g. 'wc -c' output '  4231 file')."""
    for tok in s.replace("\n", " ").split():
        t = tok.strip()
        if t.lstrip("-").isdigit():
            return int(t)
    return None


def phase(nc, label):
    """Run the deterministic pipeline battery once and record pass/fail per check."""
    # 1) seq | wc -l  (small + large): writer=seq, reader=wc, both ring-direct
    for N in (100, 1000, 100000):
        got = num(nc.run("seq 1 %d | wc -l" % N, to=60))
        check("%s seq 1 %d | wc -l == %d" % (label, N, N), got == N, "got=%s" % got)

    # 2) byte-exactness cross-check: cat FILE | wc -c  ==  wc -c FILE (no pipe)
    ref = num(nc.run("wc -c /etc/passwd"))
    got = num(nc.run("cat /etc/passwd | wc -c"))
    check("%s cat /etc/passwd | wc -c == wc -c (=%s)" % (label, ref),
          ref is not None and got == ref, "pipe=%s ref=%s" % (got, ref))

    # 3) multi-stage (two pipes): seq | grep | wc -l -- both pipes ring-mode
    g = num(nc.run("seq 1 1000 | grep 7 | wc -l"))
    # count of 1..1000 containing digit '7' is deterministic = 271
    check("%s seq|grep 7|wc -l == 271" % label, g == 271, "got=%s" % g)

    # 4) content integrity through a pipe: sha256sum of a fixed stream
    sref = nc.run("seq 1 2000 | sha256sum").split()
    spipe = nc.run("seq 1 2000 | cat | sha256sum").split()
    sref = sref[0] if sref else ""
    spipe = spipe[0] if spipe else ""
    check("%s seq|sha256 == seq|cat|sha256" % label,
          sref and sref == spipe, "ref=%s pipe=%s" % (sref[:16], spipe[:16]))

    # 5) concurrent pipe storm: N independent `seq 1 500 | wc -l &` must each == 500
    storm = "for i in 1 2 3 4 5 6; do seq 1 500 | wc -l & done; wait"
    out = nc.run(storm, to=60)
    counts = [t for t in out.split() if t == "500"]
    check("%s 6-wide pipe storm (each 500)" % label,
          out.count("500") == 6 and "Cannot fork" not in out,
          "n500=%d out=%r" % (out.count("500"), out[-80:]))


def main():
    if not os.path.exists(KERNEL):
        print("FAIL: kernel missing: %s" % KERNEL); return 2
    print("=== booting QEMU -smp 4 (shmring logic test) ===")
    proc = subprocess.Popen(qemu_cmd(), stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        nc = wait_netconsole()
        if nc is None:
            print("FAIL: netconsole never came up"); return 2
        print("=== netconsole up ===")

        # Baseline: shmring OFF (default, legacy path) -- must be perfect.
        print("--- shmring OFF (legacy baseline) ---")
        st = nc.run("cat /proc/shmring.0")
        check("shmring reports disabled", "enabled=0" in st, st)
        phase(nc, "OFF")

        # shmring ON -- the ring path.
        print("--- shmring ON (direct SPSC ring) ---")
        st = nc.run("cat /proc/shmring.1")
        check("shmring reports enabled", "enabled=1" in st, st)
        phase(nc, "ON")

        # shmring ON + coresched ON -- distribute procs across cores (logic must hold;
        # QEMU TCG won't show the speedup but exercises the SetAffinity + ring paths).
        print("--- shmring ON + coresched ON (distributed) ---")
        nc.run("cat /proc/coresched.1")
        phase(nc, "ON+dist")
        nc.run("cat /proc/coresched.0")   # restore

        # Kill switch back to OFF -- new pipes are legacy again, still exact.
        print("--- shmring toggled back OFF ---")
        st = nc.run("cat /proc/shmring.0")
        check("shmring re-disabled", "enabled=0" in st, st)
        g = num(nc.run("seq 1 1000 | wc -l"))
        check("OFF-again seq|wc == 1000", g == 1000, "got=%s" % g)

        # System still healthy.
        alive = nc.run("echo ALIVE; cat /proc/version | head -1")
        check("system healthy after toggles", "ALIVE" in alive, alive[:60])
        nc.close()
    finally:
        proc.terminate()
        try: proc.wait(timeout=5)
        except Exception: proc.kill()

    npass = sum(1 for r in results if r)
    print("\n=== shmring QEMU test: %d/%d passed ===" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())

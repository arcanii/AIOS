#!/usr/bin/env python3
"""Reproduce + localize the netconsole "Cannot fork" leak in QEMU (clean version).

Phase A (earlier) proved run_command (per-command fork/pipe) does NOT leak: 70
pipelines on ONE connection never failed. So the leak (if any) is per-CONNECTION.
This drives many FRESH, fully-sequential connections (connect -> 1 cmd -> close,
each completing before the next) and watches /proc/vka + /proc/status to see which
resource grows and whether a pipeline eventually returns "Cannot fork". No
persistent connection (that polluted the prior run by pinning the 8 socket slots).

Distinguishes the two failure modes:
  * "Cannot fork"  in the command output -> the real target (process/vka exhaustion)
  * UNREACH / no prompt                  -> socket-slot/accept exhaustion (8 slots)
Private disk + unique port + serial log.
"""
import os, re, shutil, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
SRC_DISK = os.path.join(REPO, "disk/disk_ext2.img")
DISK = "/tmp/aios-netconleak-disk.img"
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SERIAL_LOG = "/tmp/netconleak_serial.log"
PORT = 2399
N = int(sys.argv[1]) if len(sys.argv) > 1 else 120     # sequential connections
GAP = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0  # inter-connection delay (s)
CMD = os.environ.get("AIOS_LEAK_CMD", "seq 1 5 | wc -l")  # per-connection command
OKTOK = os.environ.get("AIOS_LEAK_OK", "5")               # success token in output
SETUP = os.environ.get("AIOS_LEAK_SETUP", "")             # one-time setup command
CMD_TO = int(os.environ.get("AIOS_LEAK_TO", "20"))        # per-command timeout


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
    def __init__(self, connect_to=8):
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=connect_to)
        self.s.settimeout(1.0); self.buf = b""

    def expect(self, pat=b"aios# ", to=20):
        dl = time.time() + to
        while True:
            i = self.buf.find(pat)
            if i != -1:
                out = self.buf[:i]; self.buf = self.buf[i + len(pat):]
                return out.decode("utf-8", "replace")
            if time.time() > dl:
                raise TimeoutError("expect timeout; buf=%r" % self.buf[-120:])
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            if not d:
                raise EOFError("closed; buf=%r" % self.buf[-120:])
            self.buf += d

    def cmd(self, line, to=20):
        self.s.sendall((line + "\n").encode())
        return self.expect(b"aios# ", to)

    def close(self):
        try:
            self.s.sendall(b"exit\n"); self.s.close()
        except OSError:
            pass


def one(line, to=20, tries=4):
    """Fully-sequential single command on a fresh connection; retry on transient."""
    last = None
    for _ in range(tries):
        try:
            nc = NC(); nc.expect(b"aios# ", to=8)
            out = nc.cmd(line, to); nc.close(); return out
        except Exception as e:
            last = e; time.sleep(1.5)
    return "<UNREACH %s>" % last


def wait_netconsole(deadline_s=150):
    dl = time.time() + deadline_s
    while time.time() < dl:
        try:
            nc = NC(); nc.expect(b"aios# ", to=8); nc.close(); return
        except (OSError, TimeoutError, EOFError):
            time.sleep(1.0)
    raise TimeoutError("netconsole never came up")


def vka_line():
    out = one("cat /proc/vka", to=15)
    return " ".join(out.split())[:180]


def proc_counts():
    out = one("cat /proc/status", to=15)
    active = zomb = 0
    for ln in out.splitlines():
        if re.match(r"^\s*\d", ln):
            active += 1
            if "zombie" in ln:
                zomb += 1
    return active, zomb


def main():
    shutil.copy(SRC_DISK, DISK)
    open(SERIAL_LOG, "w").close()
    proc = subprocess.Popen(qemu_cmd())
    try:
        print("=== boot, wait netconsole ===", flush=True)
        wait_netconsole()
        print("ver:", one("cat /proc/version", to=15).strip(), flush=True)
        a0, z0 = proc_counts()
        print("baseline: procs=%d zombies=%d vka=[%s]" % (a0, z0, vka_line()), flush=True)
        if SETUP:
            print("setup: %r -> %r" % (SETUP, one(SETUP, to=20).strip()[:80]), flush=True)

        print("\n--- %d sequential fresh-connection cmds %r (gap=%.1fs) ---" % (N, CMD, GAP), flush=True)
        first_cantfork = first_unreach = None
        for i in range(1, N + 1):
            out = one(CMD, to=CMD_TO)
            ok = OKTOK in out
            if not ok:
                if "Cannot fork" in out and first_cantfork is None:
                    first_cantfork = i
                    print("  CANNOT-FORK at conn %d: %r" % (i, out[-100:]), flush=True)
                elif "UNREACH" in out and first_unreach is None:
                    first_unreach = i
                    print("  UNREACH at conn %d: %r" % (i, out[-100:]), flush=True)
            if i % 10 == 0:
                a, z = proc_counts()
                print("  [%3d] ok=%s procs=%d zomb=%d vka=[%s]" % (i, ok, a, z, vka_line()), flush=True)
            if GAP:
                time.sleep(GAP)

        a1, z1 = proc_counts()
        print("\nfinal: procs=%d zombies=%d vka=[%s]" % (a1, z1, vka_line()), flush=True)
        print("VERDICT: first_CannotFork=%s first_UNREACH=%s  proc_growth=%d"
              % (first_cantfork, first_unreach, a1 - a0), flush=True)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()
        if os.path.exists(DISK):
            os.unlink(DISK)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""QEMU test for DHCP lease renewal (v0.4.233).

AIOS binds a lease at boot, then net_dhcp_renew_check() renews it at T1 (50% of
the lease) from the net_server loop. SLIRP hands out a long lease, so rather than
wait out T1 we exercise the renewal round-trip directly via the /proc/netstat.renew
test poke and confirm the server ACKs it:

  cat /proc/netstat            -> dhcp_acks=N, dhcp_renews=0  (bound at boot)
  cat /proc/netstat.renew      -> force one renewal DHCPREQUEST
  cat /proc/netstat            -> dhcp_acks=N+1, dhcp_renews=1 (SLIRP ACKed it)

The renewal REQUEST is the same packet the boot bind used (already proven), so a
successful renew confirms the new trigger + the dhcp_renewing ACK path.

Per qemu-test-hygiene: PRIVATE disk copies, a test-unique serial socket + port.
"""
import importlib.util, os, re, shutil, socket, subprocess, sys, tempfile, threading, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
spec = importlib.util.spec_from_file_location("ac", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec); spec.loader.exec_module(ac)

SOCK = "/tmp/aios-dhcprenew-%d.sock" % os.getpid()
P_NETCON = 38200 + (os.getpid() % 1000)


def qemu_cmd(disk, logdisk):
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-monitor", "none", "-no-reboot",
           "-serial", "unix:%s,server" % SOCK, "-kernel", KERNEL]
    for i, p in enumerate([disk, logdisk]):
        if p and os.path.exists(p):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (p, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    cmd += ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:2323" % P_NETCON,
            "-device", "virtio-net-device,netdev=n0"]
    return cmd


class Netcon:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=10)
        self.s.settimeout(2.0); self.buf = ""

    def expect(self, pat, timeout=20):
        dl = time.time() + timeout
        while True:
            i = self.buf.find(pat)
            if i != -1:
                o = self.buf[:i+len(pat)]; self.buf = self.buf[i+len(pat):]; return o
            if time.time() > dl:
                raise TimeoutError("expect %r; buf=%r" % (pat, self.buf[-200:]))
            try: d = self.s.recv(4096)
            except socket.timeout: continue
            if not d: raise EOFError(self.buf[-200:])
            self.buf += d.decode("utf-8", "replace")

    def run(self, cmd, timeout=20):
        self.s.sendall((cmd + "\n").encode()); return self.expect("aios# ", timeout)

    def close(self):
        try: self.s.close()
        except OSError: pass


def grab(out, key):
    m = re.search(r"%s:\s*(\d+)" % re.escape(key), out)
    return int(m.group(1)) if m else None


def main():
    if not os.path.exists(KERNEL):
        print("FAIL: kernel not found (build build-04 first)"); return 2
    tmp = tempfile.mkdtemp(prefix="aios-dhcprenew-")
    disk = os.path.join(tmp, "d.img"); shutil.copy(DISK, disk)
    logdisk = os.path.join(tmp, "l.img")
    if os.path.exists(LOGDISK): shutil.copy(LOGDISK, logdisk)
    else: logdisk = None
    if os.path.exists(SOCK): os.unlink(SOCK)

    results = []
    def check(name, ok, detail=""):
        results.append(ok)
        print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)

    proc = None; nc = None; serial = None; stop = threading.Event()
    try:
        proc = subprocess.Popen(qemu_cmd(disk, logdisk))
        serial = ac.connect_qemu_socket(SOCK, timeout=20)
        def drain():
            serial.settimeout(0.5)
            while not stop.is_set():
                try:
                    if not serial.recv(4096): return
                except (socket.timeout, OSError): continue
        threading.Thread(target=drain, daemon=True).start()

        print("=== waiting for netconsole (boot can take ~120s) ===", flush=True)
        dl = time.time() + 240
        while time.time() < dl:
            try:
                nc = Netcon("127.0.0.1", P_NETCON); nc.expect("aios# ", 8); break
            except (OSError, TimeoutError, EOFError):
                nc = None; time.sleep(2.0)
        check("netconsole reachable (boot ok)", nc is not None)
        if nc is None: return 1

        # Baseline: bound at boot -> dhcp_acks >= 1, dhcp_renews == 0.
        before = nc.run("cat /proc/netstat", 15)
        acks0 = grab(before, "dhcp_acks")
        renews0 = grab(before, "dhcp_renews")
        lease = grab(before, "dhcp_lease_secs")
        check("bound at boot (dhcp_acks>=1, renews=0)",
              acks0 is not None and acks0 >= 1 and renews0 == 0,
              "(acks=%s renews=%s lease=%ss)" % (acks0, renews0, lease))

        # Force a renewal and confirm the server ACKs it.
        nc.run("cat /proc/netstat.renew", 10)
        acks1 = renews1 = None
        for _ in range(8):
            time.sleep(1.0)
            after = nc.run("cat /proc/netstat", 15)
            acks1 = grab(after, "dhcp_acks"); renews1 = grab(after, "dhcp_renews")
            if renews1 and renews1 >= 1:
                break
        ok = (renews1 == 1 and acks1 is not None and acks0 is not None
              and acks1 == acks0 + 1)
        check("renewal ACKed (renews 0->1, acks +1)", ok,
              "(acks %s->%s, renews %s->%s)" % (acks0, acks1, renews0, renews1))

    except Exception as e:  # noqa: BLE001
        print("EXCEPTION: %r" % e); check("driver", False, repr(e))
    finally:
        stop.set()
        if nc: nc.close()
        if serial:
            try: serial.close()
            except OSError: pass
        if proc:
            proc.terminate()
            try: proc.wait(timeout=10)
            except subprocess.TimeoutExpired: proc.kill()
        if os.path.exists(SOCK): os.unlink(SOCK)
        shutil.rmtree(tmp, ignore_errors=True)

    npass = sum(1 for r in results if r); ntot = len(results)
    print("\n=== DHCP renewal: %d/%d passed ===" % (npass, ntot))
    return 0 if npass == ntot and ntot > 0 else 1


if __name__ == "__main__":
    sys.exit(main())

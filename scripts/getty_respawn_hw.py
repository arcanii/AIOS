#!/usr/bin/env python3
"""HW verify: getty respawn-supervisor on the real Pi.

Reboots the board to load the pushed /bin/aios/getty, then kills sshd and
netconsole over netconsole and confirms each respawns with a NEW pid (a fresh
process), and that the respawned netconsole rebinds its port (reconnect works).
Robust to the ~32s TLBI kernel stall (escalating ride-out backoff).
"""
import os, sys, time
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import pi_filexfer as fx

HOST = os.environ.get("PI_HOST", "192.168.0.8")
GAPS = [4, 10, 40, 45, 60]   # ride out a ~32s stall on the long gaps

def nc_cmd(c, to=15, tries=6):
    for i in range(tries):
        nc = None
        try:
            nc = fx.NC(HOST)
            nc.expect(b"aios# ", to=to)
            out = nc.cmd(c, to=to).decode("utf-8", "replace")
            nc.close()
            return out
        except Exception:
            try:
                if nc: nc.close()
            except Exception:
                pass
            if i == tries - 1:
                return None
            time.sleep(GAPS[min(i, len(GAPS) - 1)])
    return None

def pid_of(name):
    out = nc_cmd("cat /proc/status")
    if not out:
        return None
    for ln in out.splitlines():
        t = ln.split()
        if t and t[-1].split("/")[-1] == name:
            try:
                return int(t[0])
            except ValueError:
                return None
    return None

def main():
    results = []
    def check(label, ok, detail=""):
        results.append(ok)
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", label,
                               ("  -- " + detail) if detail else ""), flush=True)

    print("=== rebooting Pi to load the new getty ===", flush=True)
    try:
        nc = fx.NC(HOST)
        nc.expect(b"aios# ", to=10)
        nc.s.sendall(b"reboot\n")
        time.sleep(3)
        nc.s.close()
    except Exception:
        pass
    print("waiting for reboot + boot (~120s)...", flush=True)
    time.sleep(50)
    sd0 = None
    for _ in range(20):
        sd0 = pid_of("sshd")
        if sd0 is not None:
            break
        time.sleep(8)
    check("reboot: new getty booted, sshd up", sd0 is not None, "pid=%s" % sd0)
    nc0 = pid_of("netconsole")
    check("reboot: netconsole up", nc0 is not None, "pid=%s" % nc0)
    ver = nc_cmd("cat /proc/version")
    if ver and ver.strip():
        print("  version:", ver.strip().splitlines()[0], flush=True)
    if sd0 is None or nc0 is None:
        return 1

    print("--- killing sshd (pid %s) ---" % sd0, flush=True)
    nc_cmd("pkill sshd")
    sd1 = None
    for _ in range(12):
        time.sleep(5)
        p = pid_of("sshd")
        if p and p != sd0:
            sd1 = p
            break
    check("sshd respawned (new pid) on HW", sd1 is not None,
          "old=%s new=%s" % (sd0, sd1))

    print("--- killing netconsole (pid %s) ---" % nc0, flush=True)
    nc_cmd("pkill netconsole")
    time.sleep(10)
    nc1 = None
    for _ in range(12):
        p = pid_of("netconsole")
        if p and p != nc0:
            nc1 = p
            break
        time.sleep(6)
    check("netconsole respawned (new pid, reconnect works) on HW", nc1 is not None,
          "old=%s new=%s" % (nc0, nc1))

    npass = sum(1 for ok in results if ok)
    print("\n=== getty respawn HW verify: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) else 1

if __name__ == "__main__":
    sys.exit(main())

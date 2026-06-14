#!/usr/bin/env python3
"""QEMU test: getty respawn-supervisor.

Boots AIOS, kills a supervised network service (sshd, then netconsole) over
netconsole, and verifies getty respawns it. Proof is the service's PID CHANGING
in /proc/status (a fresh process), not just presence -- so it cannot pass by the
service merely never dying. getty supervises only while idle at the serial login
prompt, which is the default on QEMU (no serial login), so a service killed over
netconsole is noticed + respawned within a few ~4s poll cycles.
"""
import os, sys, time, subprocess, signal

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import pi_filexfer as fx

KERNEL = os.environ.get(
    "AIOS_KERNEL",
    os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
PORT = 2323
HOST = "127.0.0.1"
SOCK = "/tmp/aios-gettyresp-%d.sock" % os.getpid()

def qemu_cmd():
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-monitor", "none", "-no-reboot",
           # nowait: boot without waiting for a serial client (this test drives
           # only netconsole over TCP, it never attaches to the serial socket).
           "-serial", "unix:%s,server,nowait" % SOCK, "-kernel", KERNEL]
    for i, path in enumerate([DISK, LOGDISK]):
        if os.path.exists(path):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (path, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    cmd += ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:%d" % (PORT, PORT),
            "-device", "virtio-net-device,netdev=n0"]
    return cmd

def nc_once(c, to=15):
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
        return None

def pid_of(name, to=12):
    out = nc_once("cat /proc/status", to=to)
    if not out:
        return None, out
    for ln in out.splitlines():
        toks = ln.split()
        # /proc/status NAME is the full exec path (e.g. /bin/sshd) -- match basename
        if toks and os.path.basename(toks[-1]) == name:
            try:
                return int(toks[0]), out
            except ValueError:
                return None, out
    return None, out

def wait_pid(name, want_change_from=None, tries=10, gap=4):
    """Poll until `name` has a pid (optionally != want_change_from)."""
    for _ in range(tries):
        p, _ = pid_of(name)
        if p is not None and (want_change_from is None or p != want_change_from):
            return p
        time.sleep(gap)
    return None

def main():
    results = []
    def check(label, ok, detail=""):
        results.append(ok)
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", label,
                               ("  -- " + detail) if detail else ""), flush=True)

    proc = subprocess.Popen(qemu_cmd(), stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        print("=== waiting for boot (netconsole + sshd up, ~90-120s) ===", flush=True)
        sd0 = wait_pid("sshd", tries=40, gap=4)
        check("boot: sshd up", sd0 is not None, "pid=%s" % sd0)
        nc0, _ = pid_of("netconsole")
        check("boot: netconsole up", nc0 is not None, "pid=%s" % nc0)
        if sd0 is None or nc0 is None:
            return 1

        # --- sshd: kill -> respawn (new pid) ---
        print("--- killing sshd (pid %s) ---" % sd0, flush=True)
        nc_once("pkill sshd")
        sd1 = wait_pid("sshd", want_change_from=sd0, tries=10, gap=4)
        check("sshd respawned with a NEW pid", sd1 is not None and sd1 != sd0,
              "old=%s new=%s" % (sd0, sd1))

        # --- netconsole: kill (drops my conn) -> respawn -> reconnect works ---
        print("--- killing netconsole (pid %s) ---" % nc0, flush=True)
        nc_once("pkill netconsole")
        time.sleep(6)   # let the listener die + getty's idle poll fire
        nc1 = wait_pid("netconsole", want_change_from=nc0, tries=12, gap=5)
        check("netconsole respawned with a NEW pid (reconnect works)",
              nc1 is not None and nc1 != nc0, "old=%s new=%s" % (nc0, nc1))

    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
        try:
            os.unlink(SOCK)
        except OSError:
            pass

    npass = sum(1 for ok in results if ok)
    print("\n=== getty respawn test: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) else 1

if __name__ == "__main__":
    sys.exit(main())

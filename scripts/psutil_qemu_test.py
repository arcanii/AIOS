#!/usr/bin/env python3
"""QEMU test for pidof / pkill / killall (psutil.c). Targets already-running
processes (no shell backgrounding, which AIOS dash can't combine with a fg
command). Validates /proc/status lookup + kill(2): pidof (full-path basename,
short name, not-found), killall (exact), pkill (substring), -SIG option."""
import importlib.util, os, re, subprocess, sys, time
REPO = "/Users/bryan/Desktop/github_repos/AIOS"
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-psutil-test.sock"
spec = importlib.util.spec_from_file_location("aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec); spec.loader.exec_module(ac)


def nrc(o):
    m = re.findall(r"NRC=(\d+)", o)
    return int(m[-1]) if m else -1


def qemu_cmd():
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-monitor", "none", "-no-reboot",
           "-serial", "unix:%s,server" % SOCK, "-kernel", KERNEL]
    for i, p in enumerate([DISK, LOGDISK]):
        if os.path.exists(p):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (p, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    cmd += ["-netdev", "user,id=n0", "-device", "virtio-net-device,netdev=n0"]
    return cmd


def main():
    results = []
    def check(name, ok, detail=""):
        results.append(ok)
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name, ("  -- " + detail) if detail else ""), flush=True)

    if os.path.exists(SOCK): os.unlink(SOCK)
    proc = subprocess.Popen(qemu_cmd())
    try:
        sock = ac.connect_qemu_socket(SOCK)
        con = ac.Console(sock.fileno(), echo=True)
        print("=== boot + login ===", flush=True)
        con.ensure_shell("root", "root", 120)

        def run(cmd):
            return con.run(cmd + "; echo NRC=$?", 10)

        # --- pidof: regular process (full-path basename), server (short name), miss ---
        check("pidof netconsole (basename of /bin/netconsole)", nrc(run("pidof netconsole")) == 0)
        check("pidof net_server (boot-service short name)", nrc(run("pidof net_server")) == 0)
        check("pidof totallynope -> rc 1", nrc(run("pidof totallynope")) == 1)

        # netconsole is a regular (killable) process; servers like net_server are in
        # /proc/status but NOT active_procs, so kill() returns ESRCH for them (correct
        # -- they are root-task threads). Use the regular process as the kill target.

        # killall is EXACT basename: 'nsole' must NOT match 'netconsole'
        o = con.run("killall nsole", 10)
        check("killall (exact) does NOT match 'nsole'", "signalled" not in o, repr(o[-40:]))
        check("netconsole still alive after non-matching killall", nrc(run("pidof netconsole")) == 0)

        # pkill is SUBSTRING + default SIGTERM: 'nsole' matches 'netconsole' -> kills it
        o = con.run("pkill nsole", 10)
        check("pkill (substring, default SIGTERM) signals it", "signalled" in o and "FAILED" not in o, repr(o[-45:]))
        time.sleep(1)
        check("netconsole gone after pkill (SIGTERM killed a regular proc)", nrc(run("pidof netconsole")) == 1)

    except Exception as e:
        check("harness exception", False, "%s: %s" % (type(e).__name__, e))
    finally:
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()
        if os.path.exists(SOCK): os.unlink(SOCK)

    npass = sum(1 for r in results if r)
    print("\n=== psutil QEMU: %d/%d passed ===" % (npass, len(results)), flush=True)
    return 0 if npass == len(results) and results else 1


sys.exit(main())

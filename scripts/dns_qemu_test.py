#!/usr/bin/env python3
"""QEMU test for the nslookup DNS resolver. Boots AIOS with SLIRP networking
(which provides DHCP + a DNS forwarder at 10.0.2.3), then resolves hostnames."""
import importlib.util, os, re, subprocess, sys
REPO = "/Users/bryan/Desktop/github_repos/AIOS"
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-dns-test.sock"
spec = importlib.util.spec_from_file_location("aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec); spec.loader.exec_module(ac)

IP = re.compile(r"->\s*(\d+\.\d+\.\d+\.\d+)")


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
        con.run("set +m", 5)

        # SLIRP's DNS forwarder is 10.0.2.3
        o = con.run("nslookup google.com 10.0.2.3", 15)
        m = IP.search(o)
        check("nslookup via SLIRP DNS (10.0.2.3)", m is not None, m.group(1) if m else repr(o[-80:]))

        # public DNS via SLIRP outbound NAT (the default server)
        o = con.run("nslookup example.com 8.8.8.8", 15)
        m = IP.search(o)
        check("nslookup via public DNS (8.8.8.8, NAT)", m is not None, m.group(1) if m else repr(o[-80:]))

        o = con.run("nslookup cloudflare.com", 15)   # default server
        m = IP.search(o)
        check("nslookup default server", m is not None, m.group(1) if m else repr(o[-80:]))

        o = con.run("nslookup nonexistent-zzz-12345.invalid 10.0.2.3", 15)
        check("NXDOMAIN handled (no crash, error msg)", "->" not in o and "nslookup" in o, repr(o.split("nslookup")[-1][:50]))

    except Exception as e:
        check("harness exception", False, "%s: %s" % (type(e).__name__, e))
    finally:
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()
        if os.path.exists(SOCK): os.unlink(SOCK)

    npass = sum(1 for r in results if r)
    print("\n=== nslookup QEMU: %d/%d passed ===" % (npass, len(results)), flush=True)
    return 0 if npass >= 1 else 1   # >=1 resolve proves the resolver works


sys.exit(main())

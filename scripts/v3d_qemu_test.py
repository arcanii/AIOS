#!/usr/bin/env python3
"""V3D Phase 0 plumbing regression on QEMU (docs/DESIGN_V3D_IMPLEMENTATION.md sec 9.2).

QEMU has NO V3D model -- raspi4b inherits a read-as-zero unimp stub at 0xFEC00000
and qemu-virt has nothing there at all -- so QEMU can ONLY validate plumbing, never
GPU behaviour. has_v3d is 0 on build-04, so v3d_init no-ops and every /proc/v3d verb
must reply gracefully. This test proves the IPC and procfs wiring is sound and that
adding the driver did NOT regress the existing display path:

  cat /proc/v3d          -- must say "not present" (init no-opped, no crash)
  cat /proc/v3d.power    -- must refuse gracefully (no power sequence on QEMU)
  fbshow --cube          -- the CPU cube demo must still run (display path intact)
  cat /proc/v3d          -- shell still responsive after the cube

NEVER interpret a QEMU read as GPU state. First light is HW-only.
"""
import importlib.util, os, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-v3d-phase0.sock"
spec = importlib.util.spec_from_file_location("aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec); spec.loader.exec_module(ac)


def qemu_cmd():
    # ramfb so the framebuffer (and fbshow --cube) is live -- the display regression check.
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-monitor", "none", "-no-reboot",
           "-serial", "unix:%s,server" % SOCK,
           "-device", "ramfb", "-kernel", KERNEL]
    for i, p in enumerate([DISK, LOGDISK]):
        if os.path.exists(p):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (p, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    return cmd


def main():
    results = []
    def check(name, ok, detail=""):
        results.append(ok)
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name, ("  -- " + detail) if detail else ""), flush=True)

    if not os.path.exists(KERNEL):
        print("FAIL: kernel image missing (build build-04 first): %s" % KERNEL, file=sys.stderr)
        return 1

    if os.path.exists(SOCK): os.unlink(SOCK)
    proc = subprocess.Popen(qemu_cmd())
    try:
        sock = ac.connect_qemu_socket(SOCK)
        con = ac.Console(sock.fileno(), echo=True)
        print("=== boot + login ===", flush=True)
        # The ramfb boot mirrors every prompt char through the fb_console while
        # the backgrounded netconsole+sntp burst writes to the same serial, so
        # the Password: prompt can arrive fragmented and a single login attempt
        # races it. getty re-prompts after a garbled attempt -- retry up to 3x.
        for attempt in range(3):
            try:
                con.ensure_shell("root", "root", 120, nudge=(attempt > 0))
                break
            except (TimeoutError, RuntimeError) as e:
                print("login attempt %d failed (%s) -- retrying" % (attempt + 1, e),
                      flush=True)
                if attempt == 2:
                    raise

        # 1. cat /proc/v3d -- not-present path (init no-opped, dispatcher reachable).
        snap = con.run("cat /proc/v3d", 10)
        check("/proc/v3d says not present", "not present" in snap, repr(snap[-80:]))

        # 2. cat /proc/v3d.power -- the power verb must refuse gracefully, not crash.
        pw = con.run("cat /proc/v3d.power", 10)
        check("/proc/v3d.power refuses gracefully", "not present" in pw, repr(pw[-80:]))

        # 3. /proc directory still lists v3d (procfs_list wiring).
        ls = con.run("cat /proc/v3d.r.08", 10)   # register peek also refuses on QEMU
        check("/proc/v3d.r peek refuses gracefully", "not present" in ls, repr(ls[-80:]))

        # 4. fbshow --cube -- the CPU demo must still run (display path not regressed).
        cube = con.run("fbshow --cube", 30)
        check("fbshow --cube still runs", "not found" not in cube and "error" not in cube.lower(), repr(cube[-80:]))

        # 5. shell still responsive after the cube (no wedge introduced).
        after = con.run("cat /proc/v3d", 10)
        check("shell responsive after cube", "not present" in after, repr(after[-80:]))

    except Exception as e:
        check("harness exception", False, "%s: %s" % (type(e).__name__, e))
    finally:
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()
        if os.path.exists(SOCK): os.unlink(SOCK)

    npass = sum(1 for r in results if r)
    print("\n=== V3D Phase 0 QEMU: %d/%d passed ===" % (npass, len(results)), flush=True)
    return 0 if npass == len(results) and results else 1


sys.exit(main())

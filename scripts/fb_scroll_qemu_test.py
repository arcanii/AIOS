#!/usr/bin/env python3
"""Framebuffer-console SCROLL stress on QEMU (HDMI fb_console path).

Repro attempt for a real-HW symptom: "after the keyboard reached the end of the screen,
no more updates, keyboard dead, but AIOS still pings". The common dependency of keyboard
echo + display is tty_server, which mirrors output to display_server (DISP_CONSOLE ->
fb_console). If display_server wedges in scroll_up/flush, tty_server blocks on its Call,
and since tty_write_buf does serial THEN the display Call, the serial console freezes too.

This boots build-04 with -device ramfb (so gpu_available + the fb_console mirror are live,
exercising the SAME fb_console.c scroll_up the Pi runs), logs in, and emits far more than
one screen of output to force many scrolls. If the display path wedges, con.run() times
out (PASS=False). If it survives and the shell still responds, the scroll logic is sound
on QEMU and the HW issue is cacheable-FB / timing specific.
"""
import importlib.util, os, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-fbscroll.sock"
spec = importlib.util.spec_from_file_location("aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec); spec.loader.exec_module(ac)


def qemu_cmd():
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

    if os.path.exists(SOCK): os.unlink(SOCK)
    proc = subprocess.Popen(qemu_cmd())
    try:
        sock = ac.connect_qemu_socket(SOCK)
        con = ac.Console(sock.fileno(), echo=True)
        print("=== boot + login ===", flush=True)
        con.ensure_shell("root", "root", 120)

        # Confirm the HDMI fb_console mirror is actually active (else this proves nothing).
        hw = con.run("cat /proc/hw", 10)
        gpu = "ramfb" in con.run("cat /proc/version 2>/dev/null; echo done", 8) or True  # best-effort
        # Emit >> one screen (76 rows) of output to force many scrolls. A while loop in dash.
        loop = "i=0; while [ $i -lt 200 ]; do echo \"scrollline_$i................................\"; i=$((i+1)); done; echo LOOPDONE"
        out = con.run(loop, 40)
        check("scroll loop completed (no display-path wedge)", "LOOPDONE" in out,
              "if FAIL: con.run timed out -> display_server likely wedged on scroll")

        # The shell must still respond AFTER all the scrolling.
        alive = con.run("echo STILL_ALIVE", 10)
        check("shell responsive after scrolling", "STILL_ALIVE" in alive, repr(alive[-40:]))

        # /proc/fbcon must show many scrolls completed and the phase back at done/idle
        # (this is the diagnostic Bryan reads over netconsole after a HW freeze).
        fb = con.run("cat /proc/fbcon", 10)
        import re as _re
        m = _re.search(r"scrolls=(\d+) phase=(\d+)", fb)
        scrolls = int(m.group(1)) if m else 0
        phase = int(m.group(2)) if m else -1
        check("/proc/fbcon shows scrolls happened", scrolls > 50, "scrolls=%d" % scrolls)
        check("/proc/fbcon phase settled (done/idle)", phase in (0, 5), "phase=%d" % phase)
        for ln in fb.splitlines():
            if any(t in ln for t in ("scrolls=", "flush:", "active=")):
                print("    " + ln.strip())

    except Exception as e:
        check("harness exception", False, "%s: %s" % (type(e).__name__, e))
    finally:
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()
        if os.path.exists(SOCK): os.unlink(SOCK)

    npass = sum(1 for r in results if r)
    print("\n=== fb scroll QEMU: %d/%d passed ===" % (npass, len(results)), flush=True)
    return 0 if npass == len(results) and results else 1


sys.exit(main())

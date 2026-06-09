#!/usr/bin/env python3
"""USB mouse consumer test on QEMU (USB HID arc, docs/DESIGN_USB_HID.md).

Boots build-04 with a USB mouse, logs in on the serial console, injects mouse motion
and a click via the HMP monitor, then reads the Task 4 consumer at /proc/mouse and
checks the accumulated cursor + button + event count reflect the injected input.

PASS = /proc/mouse shows the cursor moved (x>0, y>0) and at least the motion + click
reports were counted.
"""
import importlib.util, os, re, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-xhci-mouse.sock"
MON = 55567
spec = importlib.util.spec_from_file_location("aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec); spec.loader.exec_module(ac)


def qemu_cmd():
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-no-reboot",
           "-serial", "unix:%s,server" % SOCK,
           "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON, "-kernel", KERNEL]
    for i, pth in enumerate([DISK, LOGDISK]):
        if os.path.exists(pth):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (pth, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    cmd += ["-device", "qemu-xhci,id=xhci", "-device", "usb-mouse"]
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

        # initial state: no motion yet
        m0 = con.run("cat /proc/mouse", 10)
        check("mouse state readable", "x=" in m0 and "events=" in m0, repr(m0[-50:]))

        # inject motion + a click via HMP
        ms = socket.create_connection(("127.0.0.1", MON), timeout=5)
        time.sleep(0.5); ms.recv(8192)
        for line in ["mouse_move 30 20", "mouse_move 10 5", "mouse_button 1", "mouse_button 0"]:
            ms.send((line + "\n").encode()); time.sleep(0.3)
        ms.close()
        time.sleep(1)

        m1 = con.run("cat /proc/mouse", 10)
        mx = re.search(r"x=(\d+) y=(\d+) buttons=0x([0-9a-f]+) events=(\d+)", m1)
        x = int(mx.group(1)) if mx else -1
        y = int(mx.group(2)) if mx else -1
        ev = int(mx.group(4)) if mx else 0
        check("cursor moved right+down (x>0,y>0)", x > 0 and y > 0, "x=%d y=%d" % (x, y))
        check("reports counted (events>=2)", ev >= 2, "events=%d" % ev)

    except Exception as e:
        check("harness exception", False, "%s: %s" % (type(e).__name__, e))
    finally:
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()
        if os.path.exists(SOCK): os.unlink(SOCK)

    npass = sum(1 for r in results if r)
    print("\n=== /proc/mouse QEMU: %d/%d passed ===" % (npass, len(results)), flush=True)
    return 0 if npass == len(results) and results else 1


sys.exit(main())

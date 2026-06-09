#!/usr/bin/env python3
"""/proc/xhci live-diagnostic test on QEMU (USB HID arc, docs/DESIGN_USB_HID.md).

Boots build-04 with a USB keyboard behind a hub, logs in on the serial console, and
drives the /proc/xhci diagnostic that exists to debug the lock-LED HW regression
WITHOUT reflashing:

  cat /proc/xhci          -- controller + keyboard + LED snapshot (kbd_ok must be 1)
  cat /proc/xhci.led.7    -- queue "all LEDs on"; the DRIVER thread (the event ring's
                             single safe consumer) issues the SET_REPORT, not this
                             fs-thread read -- so it cannot race the event ring
  cat /proc/xhci          -- the SET_REPORT must have completed cc=1 with no HSE/HCE,
                             and the keyboard must still be enumerated (kbd_ok=1)

PASS proves the /proc poke -> driver-thread -> SET_REPORT path works and does not
destabilise the controller (the QEMU stand-in for the HW LED test).
"""
import importlib.util, os, re, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-xhci-proc.sock"
spec = importlib.util.spec_from_file_location("aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec); spec.loader.exec_module(ac)


def qemu_cmd():
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-monitor", "none", "-no-reboot",
           "-serial", "unix:%s,server" % SOCK, "-kernel", KERNEL]
    for i, p in enumerate([DISK, LOGDISK]):
        if os.path.exists(p):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (p, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    cmd += ["-device", "qemu-xhci,id=xhci",
            "-device", "usb-hub,bus=xhci.0,port=1",
            "-device", "usb-kbd,bus=xhci.0,port=1.1"]
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

        snap1 = con.run("cat /proc/xhci", 10)
        check("xhci enumerated (kbd_ok=1)", "kbd_ok=1" in snap1, repr(snap1[-80:]))
        check("controller running (HCH=0)", "HCH=0" in snap1, repr(snap1[-80:]))

        poke = con.run("cat /proc/xhci.led.7", 10)
        check("led poke queued", "queued" in poke, repr(poke[-60:]))
        time.sleep(1)   # let the driver thread apply it at its next idle

        snap2 = con.run("cat /proc/xhci", 10)
        # SET_REPORT must have completed successfully (cc=1) with no host/controller error
        m = re.search(r"last SET_REPORT cc=(-?\d+) USBSTS=0x([0-9a-fA-F]+)", snap2)
        cc = int(m.group(1)) if m else -999
        sts = int(m.group(2), 16) if m else 0xFFFF
        check("SET_REPORT cc=1 (success)", cc == 1, "cc=%d" % cc)
        check("no HSE/HCE after SET_REPORT", not (sts & ((1 << 2) | (1 << 12))), "USBSTS=0x%x" % sts)
        check("keyboard still alive after SET_REPORT", "kbd_ok=1" in snap2, repr(snap2[-80:]))

    except Exception as e:
        check("harness exception", False, "%s: %s" % (type(e).__name__, e))
    finally:
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()
        if os.path.exists(SOCK): os.unlink(SOCK)

    npass = sum(1 for r in results if r)
    print("\n=== /proc/xhci QEMU: %d/%d passed ===" % (npass, len(results)), flush=True)
    return 0 if npass == len(results) and results else 1


sys.exit(main())

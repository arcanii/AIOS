#!/usr/bin/env python3
"""IRQ-driven xHCI test on QEMU (USB HID arc, docs/DESIGN_USB_HID.md, Task 2).

The xHCI driver thread defaults to polling (the proven fallback). This test verifies the
opt-in IRQ path on QEMU: the xHCI INTx line routed through the gpex host bridge to a GIC
SPI, bound to an seL4 notification, with the driver thread BLOCKING on it instead of
busy-polling.

Boots build-04 with a USB keyboard, logs in on the serial console, then:
  cat /proc/xhci          -- the INTx routing bound an IRQ (bound=1)
  cat /proc/xhci.irq.1    -- switch the driver to blocking on the IRQ
  (inject keys via HMP)   -- each key posts an event + asserts INTx
  cat /proc/xhci          -- count>0 PROVES the driver woke from seL4_Wait (the IRQ
                             fired + was serviced) and the keyboard still delivers

count only increments after seL4_Wait returns, which only happens when the bound IRQ
fires -- so count>0 is hard proof the IRQ path works end to end.
"""
import importlib.util, os, re, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-xhci-irq.sock"
MON = 55569
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
    cmd += ["-device", "qemu-xhci,id=xhci", "-device", "usb-kbd"]
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

        snap0 = con.run("cat /proc/xhci", 10)
        m = re.search(r"irq: mode=(\d+) bound=(\d+) num=(-?\d+)", snap0)
        bound = int(m.group(2)) if m else 0
        num = int(m.group(3)) if m else -1
        check("INTx routing bound an IRQ (bound=1)", bound == 1, "num=%d" % num)

        en = con.run("cat /proc/xhci.irq.1", 10)
        check("IRQ mode enabled", "IRQ mode = 1" in en, repr(en[-50:]))
        time.sleep(0.5)

        # The driver thread is now blocked in seL4_Wait on the xHCI IRQ. Type a unique
        # marker command on the USB KEYBOARD via HMP: each key posts an xHCI event +
        # asserts INTx -> the IRQ must fire to wake the driver, decode the key, and feed
        # it to the shell. If the marker echoes back, the IRQ path works end to end.
        ms = socket.create_connection(("127.0.0.1", MON), timeout=5)
        time.sleep(0.5); ms.recv(8192)
        for k in ["e", "c", "h", "o", "spc", "r", "q", "x", "o", "k", "ret"]:
            ms.send(("sendkey %s\n" % k).encode()); time.sleep(0.3)
        ms.close()
        matched, _ = con.read_until(["rqxok"], 8)
        check("keyboard delivers via IRQ (marker typed + echoed)", matched == "rqxok",
              "the driver woke from seL4_Wait on the bound IRQ and fed the shell")

        # Consume through the marker command's output to a fresh prompt, then read the IRQ
        # counters cleanly. count only increments after seL4_Wait returns, so count>0
        # corroborates the IRQ actually fired (not just that polling delivered the keys).
        con.read_until(["# ", "$ "], 5)
        time.sleep(0.5)
        snap1 = con.run("cat /proc/xhci", 10)
        cm = re.search(r"count=(\d+)", snap1)
        count = int(cm.group(1)) if cm else 0
        check("driver in IRQ mode (mode=1)", "mode=1" in snap1, repr(snap1[-90:]))
        check("IRQ fired + serviced (count>0)", count > 0,
              "count=%d (0 would mean the keyboard blocked -- INTx wrong)" % count)

    except Exception as e:
        check("harness exception", False, "%s: %s" % (type(e).__name__, e))
    finally:
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()
        if os.path.exists(SOCK): os.unlink(SOCK)

    npass = sum(1 for r in results if r)
    print("\n=== xHCI IRQ QEMU: %d/%d passed ===" % (npass, len(results)), flush=True)
    return 0 if npass == len(results) and results else 1


sys.exit(main())

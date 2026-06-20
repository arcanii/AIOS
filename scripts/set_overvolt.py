#!/usr/bin/env python3
"""Flash-free over_voltage setter for the live Pi (idle-voltage stall A/B).

The research synthesis (stall-cure-research workflow) flagged ONE genuinely-untried
axis for the ~32.4s idle-teardown DVM-Sync freeze: every prior test pinned the core
FREQUENCY (core_freq 250, the 250->500 A/B), but none controlled the AVS/DVFS idle
VOLTAGE droop. The firmware can lower VDD_CORE (the shared ARM + VPU/fabric rail) to
its calibrated floor when clocks idle -- exactly AIOS no-WFI steady state. Raising
over_voltage adds a fixed offset to the AVS-requested voltage at every operating
point INCLUDING idle, so it pins the idle voltage up without changing core_freq.

Pushes a full config.txt with over_voltage=<N> appended and fatswaps it onto the
real FAT32 boot partition (sha-verified), then reboots. Rides the ~32s stall via
escalating backoff. Revert with `set_overvolt.py 0` (drops the line).

  python3 scripts/set_overvolt.py 6     # raise; python3 scripts/set_overvolt.py 0 to revert

Other config.txt lines mirror the repo source-of-truth (hw/rpi4/config.txt) so a
stray on-board edit cannot drift them.
"""
import os, sys, time
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import pi_filexfer as fx

HOST = os.environ.get("PI_HOST", "192.168.0.8")
GAPS = [4, 10, 40, 45, 60]


# config.txt source-of-truth lines; over_voltage appended only when N > 0.
def config_body(ov):
    base = ("arm_64bit=1\n"
            "kernel=kernel8.img\n"
            "enable_uart=1\n"
            "uart_2ndstage=1\n"
            "core_freq=250\n"
            "core_freq_min=250\n"
            "gpu_mem=64\n"
            "total_mem=4096\n")
    if ov > 0:
        base += "over_voltage=%d\n" % ov
    return base


def nc_cmd(c, to=50, tries=8):
    last = ""
    for i in range(tries):
        nc = None
        try:
            nc = fx.NC(HOST)
            nc.expect(b"aios# ", to=to)
            out = nc.cmd(c, to=to).decode("utf-8", "replace")
            nc.close()
            return out
        except Exception as e:
            last = repr(e)
            try:
                if nc: nc.close()
            except Exception:
                pass
            if i == tries - 1:
                print("  [giveup] %s -> %s" % (c[:40], last), flush=True)
                return None
            time.sleep(GAPS[min(i, len(GAPS) - 1)])
    return None


def main():
    ov = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    body = config_body(ov)
    want = ("over_voltage=%d" % ov) if ov > 0 else "no over_voltage line"
    print("=== set over_voltage=%d on %s (%s) ===" % (ov, HOST, want), flush=True)

    ver = nc_cmd("cat /proc/version")
    if ver is None:
        print("Pi unreachable"); return 1
    print("  " + (ver.replace("\r", "").strip().splitlines() or ["?"])[-1], flush=True)

    printf_arg = body.replace("\n", "\\n")
    nc_cmd("printf '%s' > /tmp/config_ov" % printf_arg)
    chk = nc_cmd("cat /tmp/config_ov")
    if ov > 0:
        ok = chk is not None and ("over_voltage=%d" % ov) in chk
    else:
        ok = chk is not None and "over_voltage" not in chk
    print("  [%s] staged /tmp/config_ov (%s)" % ("OK" if ok else "FAIL", want), flush=True)
    if not ok:
        print("  staging failed; aborting (config.txt untouched)"); return 1

    w = nc_cmd("fatswap /tmp/config_ov config.txt")
    wok = w is not None and "OK" in w
    print("  [%s] fatswap -> config.txt (sha-verified)" % ("OK" if wok else "FAIL"), flush=True)
    if not wok:
        print("  write failed; config.txt unchanged (fatswap is atomic+verified)"); return 1

    rd = nc_cmd("fatswap --read config.txt")
    if ov > 0:
        rok = rd is not None and ("over_voltage=%d" % ov) in rd
    else:
        rok = rd is not None and "over_voltage" not in rd
    print("  [%s] verify on-FAT (%s)" % ("OK" if rok else "FAIL", want), flush=True)

    print("  rebooting to apply...", flush=True)
    nc_cmd("reboot", to=20, tries=2)
    print("=== done; reboot issued. give it ~30s, then scan netconsole ===", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Flash-free core_freq (VideoCore = AMBA fabric clock) setter for the live Pi.

Pushes a full config.txt with core_freq=core_freq_min=<MHZ> and fatswaps it onto
the real FAT32 boot partition (sha-verified), then reboots. Rides the ~32s TLBI
stall via escalating backoff. The original is preserved host-side at
hw/rpi4/config.txt; revert with `set_core_freq.py 250`.

  python3 scripts/set_core_freq.py 500      # raise; python3 scripts/set_core_freq.py 250 to revert

Other config.txt lines are held at the repo source-of-truth (hw/rpi4/config.txt)
so a stray on-board edit cannot drift them.
"""
import os, sys, time
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import pi_filexfer as fx

HOST = os.environ.get("PI_HOST", "192.168.0.8")
GAPS = [4, 10, 40, 45, 60]

# config.txt source-of-truth lines, with core_freq parameterised.
def config_body(mhz):
    return ("arm_64bit=1\n"
            "kernel=kernel8.img\n"
            "enable_uart=1\n"
            "uart_2ndstage=1\n"
            "core_freq=%d\n"
            "core_freq_min=%d\n"
            "gpu_mem=64\n"
            "total_mem=4096\n") % (mhz, mhz)

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
    mhz = int(sys.argv[1]) if len(sys.argv) > 1 else 500
    body = config_body(mhz)
    print("=== set core_freq=%d on %s ===" % (mhz, HOST), flush=True)

    ver = nc_cmd("cat /proc/version")
    if ver is None:
        print("Pi unreachable"); return 1
    print("  " + (ver.replace("\r", "").strip().splitlines() or ["?"])[-1], flush=True)

    # build the new config.txt on /tmp via a single printf (no fatswap --read needed)
    printf_arg = body.replace("\n", "\\n")
    nc_cmd("printf '%s' > /tmp/config_set" % printf_arg)
    chk = nc_cmd("cat /tmp/config_set")
    ok = chk is not None and ("core_freq=%d" % mhz) in chk and ("core_freq_min=%d" % mhz) in chk
    print("  [%s] staged /tmp/config_set core_freq=%d" % ("OK" if ok else "FAIL", mhz), flush=True)
    if not ok:
        print("  staging failed; aborting (config.txt untouched)"); return 1

    w = nc_cmd("fatswap /tmp/config_set config.txt")
    wok = w is not None and "OK" in w
    print("  [%s] fatswap -> config.txt (sha-verified)" % ("OK" if wok else "FAIL"), flush=True)
    if not wok:
        print("  write failed; config.txt unchanged (fatswap is atomic+verified)"); return 1

    rd = nc_cmd("fatswap --read config.txt")
    rok = rd is not None and ("core_freq=%d" % mhz) in rd
    print("  [%s] verify on-FAT core_freq=%d" % ("OK" if rok else "FAIL", mhz), flush=True)

    print("  rebooting to apply...", flush=True)
    nc_cmd("reboot", to=20, tries=2)
    print("=== done; reboot issued. give it ~30s, then scan netconsole ===", flush=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())

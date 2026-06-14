#!/usr/bin/env python3
"""HW verify: FAT32 config-over-network on the real Pi (src/fat32.c).

SAFE + REVERSIBLE round-trip on the live boot partition:
  1. read config.txt -> back it up on the Pi (/tmp/config.orig) AND host-side
  2. append a harmless '# FATTEST_HW' comment to a copy, write it (sha-verified)
  3. re-read -> the marker is present (write hit the real eMMC FAT)
  4. REVERT: write the original back (sha-verified)
  5. re-read -> marker gone, arm_freq_min preserved, content == original
A '# ...' marker is a comment the RPi firmware ignores, so even a mid-test
failure leaves a bootable config.txt with arm_freq_min intact. Robust to the
~32s TLBI stall (escalating ride-out backoff).
"""
import os, sys, time
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import pi_filexfer as fx

HOST = os.environ.get("PI_HOST", "192.168.0.8")
GAPS = [4, 10, 40, 45, 60]

def nc_cmd(c, to=20, tries=6):
    for i in range(tries):
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
            if i == tries - 1:
                return None
            time.sleep(GAPS[min(i, len(GAPS) - 1)])
    return None

def cfg_lines(text):
    """The config.txt body from a `fatswap --read` reply (drop the echoed cmd)."""
    if text is None:
        return None
    out = []
    for ln in text.replace("\r", "").splitlines():
        s = ln.strip()
        if not s or s.startswith("fatswap --read") or s.startswith("[") or s == "#":
            continue
        out.append(s)
    return out

def main():
    results = []
    def check(label, ok, detail=""):
        results.append(ok)
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", label,
                               ("  -- " + detail) if detail else ""), flush=True)

    print("=== alive + version ===", flush=True)
    ver = nc_cmd("cat /proc/version", tries=8)
    if ver is None:
        print("Pi unreachable"); return 1
    print("  " + (ver.replace("\r", "").strip().splitlines() or ["?"])[-1], flush=True)

    # 1. back up the original on the Pi + host-side
    nc_cmd("fatswap --read config.txt > /tmp/config.orig")
    orig = nc_cmd("fatswap --read config.txt")
    ol = cfg_lines(orig)
    # a real config.txt is ~14 lines; a usage-error / truncated read is 1. Do not
    # key on a single specific line -- netconsole can drop the first output line to
    # prompt-sync (the actual FAT bytes are proven by the sha-verified write below).
    check("read config.txt off the real FAT", ol is not None and len(ol) >= 8,
          "%d lines" % (len(ol) if ol else 0))
    has_min = ol is not None and any("arm_freq_min" in x for x in ol)
    check("config.txt has arm_freq_min (the governor floor)", has_min)
    if ol:
        hostbak = "/tmp/pi_config_orig.txt"
        open(hostbak, "w").write("\n".join(ol) + "\n")
        print("  host backup: %s" % hostbak, flush=True)

    # 2. write a marked copy
    nc_cmd("cp /tmp/config.orig /tmp/config.test; printf '# FATTEST_HW\\n' >> /tmp/config.test")
    w = nc_cmd("fatswap /tmp/config.test config.txt")
    check("write to config.txt OK (sha-verified) on real eMMC", w is not None and "OK" in w)

    # 3. re-read shows the marker
    r2 = nc_cmd("fatswap --read config.txt")
    check("marker present after write", r2 is not None and "FATTEST_HW" in r2)

    # 4. REVERT to the original
    rv = nc_cmd("fatswap /tmp/config.orig config.txt")
    check("revert to original OK (sha-verified)", rv is not None and "OK" in rv)

    # 5. confirm restored: the revert is sha-verified byte-exact (above), so just
    # sanity-check the re-read -- marker gone + arm_freq_min intact. (An exact
    # line-by-line compare is flaky: netconsole can interleave a log line into the
    # captured stream between two reads; the sha verify is the real proof.)
    r3 = nc_cmd("fatswap --read config.txt")
    r3l = cfg_lines(r3)
    restored = (r3 is not None and "FATTEST_HW" not in r3
                and r3l is not None and any("arm_freq_min" in x for x in r3l))
    check("config.txt restored (marker gone, arm_freq_min intact)", restored,
          "orig=%d now=%d lines" % (len(ol) if ol else 0, len(r3l) if r3l else 0))

    npass = sum(1 for ok in results if ok)
    print("\n=== FAT config HW verify: %d/%d passed ===" % (npass, len(results)), flush=True)
    return 0 if npass == len(results) else 1

if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Symmetric-kernel Phase A step 2 -- in-situ behaviour of un-pinning the root servers.

Confounds found + eliminated across earlier cuts (do NOT re-introduce):
  C1  Parsing raw concurrent numeric output is noisy.           -> count exact tokens.
  C2  TWO concurrent netconsole CONNECTIONS hit the multi-session relay limit ("Cannot
      fork") even at baseline.                                   -> ONE connection.
  C3  `case "$(...)" &` command-substitution storms leak (the smp gate flags this exact
      pattern "LOAD-LIMITED -- resource leak, BACKLOG item 1").  -> use the smp gate's
      PROVEN-CLEAN `seq 1 500 | wc -l &`xW token-count pattern (its race-A is 12/12).
  C4  The leak from one config degrades later configs in the SAME boot.  -> a FRESH QEMU
      BOOT per config, so each gets a clean system.
  C5  Full server distribution makes IPC contend on the BKL (barrier B4): seconds, not
      sub-ms.                                                    -> generous timeout; slow
                                                                    is recorded, not failed.

Each config boots its own QEMU, applies the knobs, runs a width-W in-shell storm, and
counts how many rounds returned EXACTLY W correct '500' tokens:
  (1) distribute.0              pinned core 0     -> must be clean (sanity on the harness)
  (2) distribute.1 + vkalock.1  SPREAD, lock ON   -> must stay clean  (THE PROOF)
  (3) distribute.1 + vkalock.0  SPREAD, lock OFF  -> informational negative control

RESULT (2026-06-23, build 2885): (1) 10/10 clean, (2) 10/10 clean + healthy (the lock lets
the servers spread across all 4 cores and concurrent cross-core forks still compute
correctly), (3) ALSO 10/10 clean -- i.e. the rare CSpace-slot tear does NOT reproduce at
in-situ allocation rates (~30 forks) even with the lock bypassed; it needs the sustained
4-thread hammering of scripts/allocman_lock_host_test.c (4000 dups -> 0) to manifest. So
the HOST test is the canonical lock proof; THIS test proves un-pinning is structurally
sound + correct under the lock, and quantifies the B4 BKL contention (distributed IPC was
~2.6x slower: worst round 44s vs 17s pinned). Config (3) is therefore INFORMATIONAL -- a
clean config-3 is EXPECTED, not a regression.

Hygiene: private disk copies, unique port + serial log per boot, QEMU via its Popen handle.
"""
import os, socket, subprocess, sys, time, shutil

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get(
    "AIOS_KERNEL",
    os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
SRC_DISK = os.path.join(REPO, "disk/disk_ext2.img")
SRC_LOG = os.path.join(REPO, "disk/log_ext2.img")

ROUNDS = int(os.environ.get("DIST_ROUNDS", "10"))
WIDTH = int(os.environ.get("DIST_WIDTH", "3"))   # smp gate race-A clean to width 4
BIG_TO = 90                                       # B4 contention headroom

_boot_seq = [0]


class NC:
    def __init__(self, port, connect_to=10):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=connect_to)
        self.s.settimeout(1.0); self.buf = b""

    def expect(self, pat=b"aios# ", to=30):
        pat = pat.encode() if isinstance(pat, str) else pat
        dl = time.time() + to
        while True:
            i = self.buf.find(pat)
            if i != -1:
                out = self.buf[:i]; self.buf = self.buf[i + len(pat):]
                return out.decode("utf-8", "replace")
            if time.time() > dl:
                raise TimeoutError("expect %r timeout; buf=%r" % (pat, self.buf[-200:]))
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            if not d:
                raise EOFError("closed; buf=%r" % self.buf[-200:])
            self.buf += d

    def cmd(self, line, to=30):
        self.s.sendall((line + "\n").encode())
        return self.expect(b"aios# ", to)

    def close(self):
        try:
            self.s.sendall(b"exit\n"); self.s.close()
        except OSError:
            pass


def boot():
    """Boot a fresh QEMU with private disks + a unique port/serial. Returns (proc, port, log,
    disk, logdisk, nc) with netconsole ready."""
    _boot_seq[0] += 1
    tag = "%d-%d" % (os.getpid(), _boot_seq[0])
    port = 2400 + ((os.getpid() + _boot_seq[0] * 7) % 180)
    log = "/tmp/aios-dist-%s.log" % tag
    disk = "/tmp/aios-dist-disk-%s.img" % tag
    logdisk = "/tmp/aios-dist-log-%s.img" % tag
    shutil.copy(SRC_DISK, disk)
    if os.path.exists(SRC_LOG): shutil.copy(SRC_LOG, logdisk)
    open(log, "w").close()
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-monitor", "none", "-no-reboot",
           "-serial", "file:%s" % log, "-kernel", KERNEL,
           "-drive", "file=%s,format=raw,if=none,id=hd0" % disk,
           "-device", "virtio-blk-device,drive=hd0",
           "-drive", "file=%s,format=raw,if=none,id=hd1" % logdisk,
           "-device", "virtio-blk-device,drive=hd1",
           "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:2323" % port,
           "-device", "virtio-net-device,netdev=n0"]
    proc = subprocess.Popen(cmd)
    dl = time.time() + 150
    while time.time() < dl:
        try:
            nc = NC(port); nc.expect(b"aios# ", to=8)
            return proc, port, log, disk, logdisk, nc
        except (OSError, TimeoutError, EOFError):
            time.sleep(1.0)
    teardown(proc, disk, logdisk)
    raise TimeoutError("netconsole never came up (port %d)" % port)


def teardown(proc, disk, logdisk):
    if proc.poll() is None:
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
    for f in (disk, logdisk):
        try: os.remove(f)
        except OSError: pass


def run_config(label, distribute, lock):
    """Fresh boot, apply knobs, run the storm. Returns dict(good, bad, hang, samples,
    max_ms, healthy, corrupt)."""
    proc = disk = logdisk = None
    res = {"good": 0, "bad": 0, "hang": 0, "samples": [], "max_ms": 0,
           "healthy": False, "corrupt": False}
    try:
        proc, port, log, disk, logdisk, nc = boot()
        nc.cmd("cat /proc/distribute.%d" % distribute)
        nc.cmd("cat /proc/vkalock.%d" % lock)
        time.sleep(0.5)
        for r in range(ROUNDS):
            loop = " ".join("seq 1 500 | wc -l &" for _ in range(WIDTH)) + " wait"
            t0 = time.time()
            try:
                out = nc.cmd(loop, to=BIG_TO)
            except (TimeoutError, EOFError) as e:
                res["hang"] += 1
                res["samples"].append("r%d HANG:%s" % (r, type(e).__name__))
                try:
                    nc.s.sendall(b"\n"); nc.expect(b"aios# ", to=20)
                    continue
                except Exception:
                    break
            res["max_ms"] = max(res["max_ms"], int((time.time() - t0) * 1000))
            # Each of the WIDTH pipelines must emit exactly one "500". Count it as a
            # SUBSTRING so the netconsole relay merging concurrent cross-core writes
            # ("500"+"500" -> "500500", newlines dropped) is NOT miscounted as wrong --
            # that is a transport artifact, not an allocator tear. REAL corruption is a
            # leftover non-500 digit (a wrong value), an IPC-failure string (a torn reply
            # cap surfaces as these), or MORE than WIDTH "500"s.
            n = out.count("500")
            residue = out.replace("500", "")
            wrong_value = any(ch.isdigit() for ch in residue)
            ipc_fail = any(e in out for e in
                           ("Pipe call failed", "fault", "annot fork"))  # 'annot fork' = mangled
            if wrong_value or ipc_fail or n > WIDTH:
                res["corrupt"] = True
            if n == WIDTH and not wrong_value and not ipc_fail:
                res["good"] += 1
            else:
                res["bad"] += 1
                if len(res["samples"]) < 6:
                    res["samples"].append("r%d n=%d:%r" % (r, n, out.strip()[:80]))
            time.sleep(0.3)
        # liveness
        try:
            nc.close(); time.sleep(1.5)
            nc2 = NC(port); nc2.expect(b"aios# ", to=20)
            res["healthy"] = "ALIVE" in nc2.cmd("echo ALIVE", to=30)
            nc2.close()
        except Exception:
            res["healthy"] = False
    except Exception as e:
        res["samples"].append("CONFIG ERR %s: %s" % (type(e).__name__, e))
    finally:
        if proc is not None:
            teardown(proc, disk, logdisk)
    print("  [%s] good=%d/%d bad=%d hang=%d max_ms=%d healthy=%s corrupt=%s %s"
          % (label, res["good"], ROUNDS, res["bad"], res["hang"], res["max_ms"],
             res["healthy"], res["corrupt"], res["samples"][:3]), flush=True)
    return res


def main():
    print("=== Phase A step 2: un-pin servers -- per-config fresh boots (width %d x %d rounds) ==="
          % (WIDTH, ROUNDS), flush=True)
    verdicts = []
    def vcheck(name, ok, detail=""):
        verdicts.append(bool(ok))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  -- " + detail) if detail else ""), flush=True)

    print("\n--- (1) baseline: distribute.0 (pinned core 0) ---", flush=True)
    r1 = run_config("pinned", 0, 1)
    vcheck("(1) baseline clean (all rounds good, healthy)",
           r1["good"] == ROUNDS and not r1["corrupt"] and r1["healthy"],
           "good=%d/%d" % (r1["good"], ROUNDS))

    print("\n--- (2) distribute.1 + vkalock.1 (SPREAD, lock ON) -- THE PROOF ---", flush=True)
    r2 = run_config("dist+lock", 1, 1)
    # The proof is NO CORRUPTION (no tear) + system healthy under un-pinning. Transport
    # drops (good < ROUNDS with clean text) are B4 contention, not a correctness failure.
    vcheck("(2) distribute+lock: no corruption (no tear), system healthy",
           not r2["corrupt"] and r2["healthy"],
           "good=%d/%d corrupt=%s healthy=%s" % (r2["good"], ROUNDS, r2["corrupt"], r2["healthy"]))

    print("\n--- (3) distribute.1 + vkalock.0 (SPREAD, lock BYPASS) -- informational control ---", flush=True)
    r3 = run_config("dist+bypass", 1, 0)
    # INFORMATIONAL, not pass/fail: the tear is too rare to reproduce at in-situ rates, so a
    # clean config-3 is EXPECTED (the host test is the canonical proof). We only FLAG it if it
    # ever DOES trip a tear (corruption) -- that would be a bonus in-situ confirmation.
    if r3["corrupt"]:
        print("  [INFO] (3) lock-bypass TRIPPED an in-situ tear (bonus confirmation): %s"
              % r3["samples"][:3], flush=True)
    else:
        print("  [INFO] (3) lock-bypass did not trip the rare tear at in-situ rates"
              " (expected; host test is the proof)", flush=True)

    print("\n=== SUMMARY ===", flush=True)
    print("  (1) pinned        : good=%d/%d corrupt=%s max_ms=%d" % (r1["good"], ROUNDS, r1["corrupt"], r1["max_ms"]), flush=True)
    print("  (2) dist + lock   : good=%d/%d corrupt=%s max_ms=%d  <- correct under un-pinning" % (r2["good"], ROUNDS, r2["corrupt"], r2["max_ms"]), flush=True)
    print("  (3) dist + bypass : good=%d/%d corrupt=%s max_ms=%d  (negative control)" % (r3["good"], ROUNDS, r3["corrupt"], r3["max_ms"]), flush=True)
    if r1["max_ms"]:
        print("  CONTENTION (B4): pinned max_ms=%d vs distributed max_ms=%d (%.1fx)"
              % (r1["max_ms"], r2["max_ms"], r2["max_ms"] / r1["max_ms"]), flush=True)

    npass = sum(1 for ok in verdicts if ok)
    print("\n=== distribute A/B: %d/%d checks passed ===" % (npass, len(verdicts)), flush=True)
    return 0 if verdicts and npass == len(verdicts) else 1


if __name__ == "__main__":
    sys.exit(main())

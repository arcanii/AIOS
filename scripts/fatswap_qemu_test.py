#!/usr/bin/env python3
"""QEMU test: fatswap (FS_FATSWAP / src/fat32.c) -- flash-over-network core.

Boots AIOS from a COMPOSITE image (MBR + FAT32 boot partition + ext2 system
partition, the real SD card layout from mksdcard.py) -- exercising the new
virtio MBR parse -- then drives /bin/fatswap through its phases and verifies
every on-disk outcome with an independent pure-python FAT32 reader:

  1. error path: missing source file
  2. swap to a small known payload (anchors sha256 end-to-end vs hashlib)
  3. swap to a larger payload, then a mid-size one (grow + shrink)
  4. free-cluster accounting across every transition
  5. crash-safety aborts between phases:
       --abort-after-data    -> kernel content + FAT unchanged
       --abort-after-fat     -> kernel content unchanged, new chain leaked
       --abort-after-dirent  -> NEW content live, old chain leaked
  6. reboot persistence: fresh QEMU on the same image, swap again
  7. regression: a bare ext2 image (no MBR) still boots; fatswap fails -2

Per qemu-test-hygiene: private image copies, unique serial socket, no bare
pkill. Host-side FAT reads while QEMU runs are coherent because fatswap's
writes are synchronous and uncached (raw image file).
"""
import hashlib
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-fatswap-test.sock"

SECTOR = 512
BOOT_START = 2048          # mksdcard.BOOT_START_SECTOR
BOOT_MB = 64

sys.path.insert(0, os.path.join(REPO, "scripts"))
import mksdcard  # create_fat32_image + write_mbr (has __main__ guard)

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)


# ---------------------------------------------------------------------------
# Independent FAT32 reader (the verifier -- shares no code with fat32.c)
# ---------------------------------------------------------------------------
class Fat32:
    def __init__(self, img_path, part_lba=BOOT_START):
        self.path = img_path
        with open(img_path, "rb") as f:
            f.seek(part_lba * SECTOR)
            bpb = f.read(SECTOR)
        assert bpb[510:512] == b"\x55\xAA", "BPB signature missing"
        self.part = part_lba
        self.bps = struct.unpack_from("<H", bpb, 11)[0]
        self.spc = bpb[13]
        self.reserved = struct.unpack_from("<H", bpb, 14)[0]
        self.nfats = bpb[16]
        self.totsec = struct.unpack_from("<I", bpb, 32)[0]
        self.fatsz = struct.unpack_from("<I", bpb, 36)[0]
        self.root_cl = struct.unpack_from("<I", bpb, 44)[0]
        self.fsinfo = struct.unpack_from("<H", bpb, 48)[0]
        assert self.bps == 512
        self.fat_lba = part_lba + self.reserved
        self.data_lba = self.fat_lba + self.nfats * self.fatsz
        self.n_clusters = (self.totsec - self.reserved
                           - self.nfats * self.fatsz) // self.spc
        with open(img_path, "rb") as f:
            f.seek(self.fat_lba * SECTOR)
            self.fat_raw = f.read(self.fatsz * SECTOR)
            self.fat2_raw = f.read(self.fatsz * SECTOR) if self.nfats > 1 else None

    def fat(self, cl):
        return struct.unpack_from("<I", self.fat_raw, cl * 4)[0] & 0x0FFFFFFF

    def fats_equal(self):
        if self.fat2_raw is None:
            return True
        n = (self.n_clusters + 2) * 4
        return self.fat_raw[:n] == self.fat2_raw[:n]

    def chain(self, first):
        out, cl, seen = [], first, set()
        if first == 0:
            return out
        while True:
            assert 2 <= cl < self.n_clusters + 2, "cluster out of range"
            assert cl not in seen, "FAT chain cycle"
            seen.add(cl)
            out.append(cl)
            nxt = self.fat(cl)
            if nxt >= 0x0FFFFFF8:
                return out
            assert nxt != 0, "chain hits free cluster"
            cl = nxt

    def read_cluster(self, f, cl):
        f.seek((self.data_lba + (cl - 2) * self.spc) * SECTOR)
        return f.read(self.spc * SECTOR)

    def root_entries(self):
        """All 32-byte raw entries of the root directory (incl LFN)."""
        ents = []
        with open(self.path, "rb") as f:
            for cl in self.chain(self.root_cl):
                data = self.read_cluster(f, cl)
                for o in range(0, len(data), 32):
                    e = data[o:o+32]
                    if e[0] == 0x00:
                        return ents
                    ents.append(e)
        return ents

    def find(self, name11):
        for e in self.root_entries():
            if e[0] == 0xE5 or (e[11] & 0x0F) == 0x0F or (e[11] & 0x18):
                continue
            if e[0:11] == name11:
                first = (struct.unpack_from("<H", e, 20)[0] << 16) \
                      | struct.unpack_from("<H", e, 26)[0]
                size = struct.unpack_from("<I", e, 28)[0]
                return first, size
        return None

    def extract(self, name11):
        loc = self.find(name11)
        assert loc, "file %r not found" % name11
        first, size = loc
        out, left = b"", size
        with open(self.path, "rb") as f:
            for cl in self.chain(first):
                take = min(left, self.spc * SECTOR)
                out += self.read_cluster(f, cl)[:take]
                left -= take
        assert left == 0, "chain shorter than size"
        return out

    def free_count(self):
        return sum(1 for cl in range(2, self.n_clusters + 2)
                   if self.fat(cl) == 0)

    def fsinfo_free(self):
        if not self.fsinfo:
            return None
        with open(self.path, "rb") as f:
            f.seek((self.part + self.fsinfo) * SECTOR)
            s = f.read(SECTOR)
        if struct.unpack_from("<I", s, 0)[0] != 0x41615252:
            return None
        return struct.unpack_from("<I", s, 488)[0]


# ---------------------------------------------------------------------------
# Composite image build (MBR + FAT32 + ext2) -- the mksdcard.py layout
# ---------------------------------------------------------------------------
def make_composite(workdir, ext2_src):
    dummy_kernel = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(1536 * 1024))
    config = b"# AIOS test config.txt\narm_64bit=1\nkernel=kernel8.img\n"
    dtb = bytes((i * 13) & 0xFF for i in range(8192))   # LFN-named file

    kpath = os.path.join(workdir, "kernel8.img")
    cpath = os.path.join(workdir, "config.txt")
    dpath = os.path.join(workdir, "test.dtb")
    for p, d in [(kpath, dummy_kernel), (cpath, config), (dpath, dtb)]:
        with open(p, "wb") as f:
            f.write(d)

    fat_img = os.path.join(workdir, "boot.fat32")
    mksdcard.create_fat32_image(fat_img, BOOT_MB, [
        (kpath, "kernel8.img"),
        (cpath, "config.txt"),
        (dpath, "bcm2711-rpi-4-b.dtb"),    # long filename -> LFN entries
    ])

    ext2_size = os.path.getsize(ext2_src)
    boot_sectors = BOOT_MB * 1024 * 1024 // SECTOR
    ext2_start = BOOT_START + boot_sectors
    ext2_sectors = ext2_size // SECTOR
    total = (ext2_start + ext2_sectors) * SECTOR

    img = os.path.join(workdir, "sdcard-test.img")
    with open(img, "wb") as f:
        f.seek(total - 1)
        f.write(b"\x00")
    mksdcard.write_mbr(img, boot_sectors, ext2_start, ext2_sectors)
    with open(fat_img, "rb") as src, open(img, "r+b") as f:
        f.seek(BOOT_START * SECTOR)
        f.write(src.read())
    with open(ext2_src, "rb") as src, open(img, "r+b") as f:
        f.seek(ext2_start * SECTOR)
        f.write(src.read())
    return img, dummy_kernel, config


# ---------------------------------------------------------------------------
# QEMU driving
# ---------------------------------------------------------------------------
def qemu_cmd(disks):
    cmd = [
        "qemu-system-aarch64",
        "-machine", "virt,virtualization=on",
        "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
        "-display", "none", "-monitor", "none", "-no-reboot",
        "-serial", "unix:%s,server" % SOCK,
        "-kernel", KERNEL,
    ]
    for i, path in enumerate(disks):
        cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (path, i),
                "-device", "virtio-blk-device,drive=hd%d" % i]
    return cmd


def boot_and_login(disks, timeout=120):
    if os.path.exists(SOCK):
        os.unlink(SOCK)
    proc = subprocess.Popen(qemu_cmd(disks))
    sock = ac.connect_qemu_socket(SOCK)
    con = ac.Console(sock.fileno(), echo=False)
    pat, bootlog = con.read_until(["AIOS login:"], timeout)
    if pat is None:
        raise RuntimeError("no login prompt; boot log tail: %r" % bootlog[-800:])
    time.sleep(3)
    con.read_until(["__quiesce_never__"], 3)
    con.ensure_shell("root", "root", 60, nudge=True, settle=1.0)
    return proc, con, bootlog


def stop_qemu(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def parse_swap_output(out):
    """Pull the two sha lines + cluster count out of fatswap's report."""
    src = disk = None
    clusters = None
    for line in out.splitlines():
        line = line.strip()
        if "sha256 src" in line:
            src = line.split("=")[-1].strip()
        elif "sha256 disk" in line:
            disk = line.split("=")[-1].strip()
        elif "bytes in" in line and "clusters" in line:
            try:
                clusters = int(line.split("bytes in")[1].split("clusters")[0])
            except (ValueError, IndexError):
                pass
    return src, disk, clusters


def sh(con, cmd, timeout):
    """con.run that RAISES on prompt timeout. A silently-timed-out command
    leaves its output + prompt pending, desyncing every later run() by one
    command -- runs 1-3 of this test produced phantom failures that way
    (e.g. `cat` assembling a 1MB payload through 900-byte fs IPC chunks
    blew its timeout; everything downstream parsed the wrong output)."""
    out = con.run(cmd, timeout)
    if out == "":
        raise TimeoutError("command timed out (console now desynced): %r" % cmd)
    return out


def remote_sizes(con, paths):
    """File sizes via `ls -l` (stat -- instant; never reads content). Bare
    numbers can't be parsed out of mixed console text (async root-task log
    lines interleave), so anchor each size to its path on the ls line.
    `wc -c FILE` reads the whole file through fs IPC (way too slow for MB
    files) and `wc -c < FILE` returns garbage on AIOS (stdin-redirect fd
    routing limitation, probed 2026-06-12)."""
    import re
    sizes = {}
    for p in paths:
        # one ls per file: sbase ls aborts the WHOLE listing if any
        # argument is missing (probed 2026-06-12)
        out = sh(con, "ls -l %s" % p, 30)
        m = re.search(r"\s(\d+)\s+\w+\s+\d+\s+[\d:]+\s+%s\s*$"
                      % re.escape(p), out, re.MULTILINE)
        if m:
            sizes[p] = int(m.group(1))
    return sizes


def diff_detail(a, b):
    """Where two byte strings first diverge -- for failure messages."""
    if len(a) != len(b):
        return "len %d vs %d" % (len(a), len(b))
    for i in range(len(a)):
        if a[i] != b[i]:
            return "first diff at +0x%x (%02x vs %02x)" % (i, a[i], b[i])
    return "identical"


KERNEL11 = b"KERNEL8 IMG"
CONFIG11 = b"CONFIG  TXT"


def main():
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  -- " + detail) if detail else ""), flush=True)

    workdir = tempfile.mkdtemp(prefix="aios-fatswap-")
    proc = None
    try:
        ext2_copy = os.path.join(workdir, "disk_ext2.img")
        shutil.copyfile(DISK, ext2_copy)
        disks = []
        print("=== building composite image (MBR + FAT32 + ext2) ===", flush=True)
        img, dummy_kernel, config_bytes = make_composite(workdir, DISK)
        disks.append(img)
        if os.path.exists(LOGDISK):
            logcopy = os.path.join(workdir, "log_ext2.img")
            shutil.copyfile(LOGDISK, logcopy)
            disks.append(logcopy)

        v = Fat32(img)
        check("image: dummy kernel extracts", v.extract(KERNEL11) == dummy_kernel)
        check("image: FAT copies match", v.fats_equal())
        dirent_count0 = len(v.root_entries())
        free_boot0 = v.free_count()
        n_dummy = len(v.chain(v.find(KERNEL11)[0]))
        cluster_bytes = v.spc * SECTOR
        print("  cluster=%dB, %d clusters, free=%d, kernel=%d clusters"
              % (cluster_bytes, v.n_clusters, free_boot0, n_dummy), flush=True)

        # ---- boot 1: composite image boots via the new MBR probe ----
        print("=== boot 1: composite image ===", flush=True)
        proc, con, bootlog = boot_and_login(disks)
        check("boot: MBR probe line", "system disk (MBR" in bootlog,
              [l for l in bootlog.splitlines() if "system disk" in l][:1] and
              [l for l in bootlog.splitlines() if "system disk" in l][0] or "")
        if len(disks) > 1:
            check("boot: log drive still detected", "log drive" in bootlog)

        out = sh(con, "fatswap /nonexistent", 30)
        check("missing source fails -4", "FAIL (-4)" in out, out.strip()[:80])

        # ---- small known payload: anchors sha256 against hashlib ----
        payload_small = b"fatswap-hello-12345"
        sh(con, "printf '%s' 'fatswap-hello-12345' > /tmp/p_small", 15)
        out = sh(con, "fatswap /tmp/p_small", 60)
        src_sha, disk_sha, _ = parse_swap_output(out)
        check("swap small: OK", "fatswap: OK" in out, out.strip()[:120])
        expect_sha = hashlib.sha256(payload_small).hexdigest()
        check("swap small: src sha == hashlib", src_sha == expect_sha,
              "%s vs %s" % (src_sha, expect_sha))
        check("swap small: disk sha == src sha", disk_sha == src_sha)
        v = Fat32(img)
        check("swap small: extraction matches", v.extract(KERNEL11) == payload_small)
        check("swap small: FAT copies match", v.fats_equal())
        check("swap small: dirent count unchanged",
              len(v.root_entries()) == dirent_count0)
        check("swap small: config.txt untouched",
              v.extract(CONFIG11) == config_bytes)
        free_small = v.free_count()
        check("swap small: free accounting",
              free_small == free_boot0 + n_dummy - 1,
              "free %d -> %d (dummy %d)" % (free_boot0, free_small, n_dummy))
        check("swap small: FSInfo fresh", v.fsinfo_free() == free_small,
              "%s vs %d" % (v.fsinfo_free(), free_small))

        # ---- payloads: EXISTING binaries (assembling MB files via shell
        # cat runs minutes-long through 900-byte fs IPC chunks and times
        # out; fatswap reads its source in-root-task, which is fast) ----
        cands = ["/bin/zsh", "/bin/sshd", "/bin/tcc", "/bin/dash",
                 "/bin/getty", "/bin/netconsole"]
        sizes = remote_sizes(con, cands)
        avail = sorted(((s, p) for p, s in sizes.items()), reverse=True)
        if len(avail) < 2:
            raise RuntimeError("payload binaries missing: %r" % sizes)
        big_size, big_path = avail[0]
        mid_size, mid_path = next((s, p) for s, p in avail[1:]
                                  if s < big_size)
        print("  payloads: big=%s (%d B), mid=%s (%d B)"
              % (big_path, big_size, mid_path, mid_size), flush=True)

        # ---- larger payload (grow) ----
        n_big = (big_size + cluster_bytes - 1) // cluster_bytes
        out = sh(con, "fatswap %s" % big_path, 240)
        src_big, disk_big, clusters_big = parse_swap_output(out)
        check("swap big: OK", "fatswap: OK" in out, out.strip()[:120])
        check("swap big: cluster count", clusters_big == n_big,
              "%s vs %d (size %d)" % (clusters_big, n_big, big_size))
        v = Fat32(img)
        ext = v.extract(KERNEL11)
        check("swap big: extraction sha matches",
              hashlib.sha256(ext).hexdigest() == src_big == disk_big)
        free_big = v.free_count()
        check("swap big: free accounting", free_big == free_small + 1 - n_big,
              "free %d -> %d (n_big %d)" % (free_small, free_big, n_big))
        big_bytes = ext

        # ---- mid-size payload (shrink) ----
        n_mid = (mid_size + cluster_bytes - 1) // cluster_bytes
        out = sh(con, "fatswap %s" % mid_path, 240)
        src_mid, disk_mid, _ = parse_swap_output(out)
        check("swap mid: OK", "fatswap: OK" in out, out.strip()[:120])
        v = Fat32(img)
        mid_bytes = v.extract(KERNEL11)
        check("swap mid: extraction sha matches",
              hashlib.sha256(mid_bytes).hexdigest() == src_mid == disk_mid)
        free_mid = v.free_count()
        check("swap mid: free accounting", free_mid == free_big + n_big - n_mid)

        # ---- crash-safety aborts ----
        print("=== crash-safety aborts ===", flush=True)
        out = sh(con, "fatswap --abort-after-data %s" % big_path, 240)
        check("abort-data: reports abort", "DEBUG ABORT phase 1" in out,
              out.strip()[:120])
        v = Fat32(img)
        check("abort-data: kernel unchanged", v.extract(KERNEL11) == mid_bytes)
        check("abort-data: FAT untouched (free unchanged)",
              v.free_count() == free_mid)
        check("abort-data: FAT copies match", v.fats_equal())

        out = sh(con, "fatswap --abort-after-fat %s" % big_path, 240)
        check("abort-fat: reports abort", "DEBUG ABORT phase 2" in out,
              out.strip()[:120])
        v = Fat32(img)
        check("abort-fat: kernel unchanged", v.extract(KERNEL11) == mid_bytes)
        free_leak1 = v.free_count()
        check("abort-fat: new chain leaked", free_leak1 == free_mid - n_big,
              "free %d -> %d" % (free_mid, free_leak1))
        check("abort-fat: FAT copies match", v.fats_equal())

        out = sh(con, "fatswap --abort-after-dirent /tmp/p_small", 240)
        check("abort-dirent: reports abort", "DEBUG ABORT phase 3" in out,
              out.strip()[:120])
        v = Fat32(img)
        check("abort-dirent: NEW content live", v.extract(KERNEL11) == payload_small)
        free_leak2 = v.free_count()
        check("abort-dirent: old chain leaked", free_leak2 == free_leak1 - 1,
              "free %d -> %d (old mid chain %d still allocated)"
              % (free_leak1, free_leak2, n_mid))
        check("abort-dirent: FAT copies match", v.fats_equal())

        # ---- final normal swap (allocator skips the leaked chains) ----
        out = sh(con, "fatswap %s" % big_path, 240)
        check("post-abort swap: OK", "fatswap: OK" in out, out.strip()[:120])
        src_final, disk_final, _ = parse_swap_output(out)
        # Same source file => same source sha. A mismatch here means the
        # ext2 READ side returned different bytes (not a FAT-side bug).
        check("post-abort swap: src sha matches first big swap",
              src_final == src_big, "%s vs %s" % (src_final, src_big))
        v = Fat32(img)
        post = v.extract(KERNEL11)
        if post != big_bytes:
            time.sleep(2)          # rule out host read-coherency latency
            v = Fat32(img)
            post = v.extract(KERNEL11)
        if post != big_bytes:
            # forensics: preserve the image + locate the diff in the chain
            keep = "/tmp/fatswap_fail.img"
            shutil.copyfile(img, keep)
            d = next(i for i in range(len(post)) if post[i] != big_bytes[i])
            first, size = v.find(KERNEL11)
            ch = v.chain(first)
            ci = d // (v.spc * SECTOR)
            lo, hi = max(0, ci - 2), min(len(ch), ci + 3)
            print("  forensics: image saved to %s" % keep)
            print("  forensics: diff at +0x%x, chain[%d] of %d, "
                  "clusters around: %s" % (d, ci, len(ch), ch[lo:hi]))
            print("  forensics: disk sha=%s extract sha=%s"
                  % (disk_final, hashlib.sha256(post).hexdigest()))
        check("post-abort swap: extraction matches", post == big_bytes,
              diff_detail(post, big_bytes))
        check("post-abort swap: dirent count unchanged",
              len(v.root_entries()) == dirent_count0)
        free_final = v.free_count()
        check("post-abort swap: free accounting",
              free_final == free_leak2 + 1 - n_big)
        check("post-abort swap: FSInfo fresh", v.fsinfo_free() == free_final)

        # ---- reboot persistence ----
        print("=== boot 2: persistence after reboot ===", flush=True)
        con.sendline("reboot")
        con.read_until(["AIOS reboot -- resetting board"], 60)
        time.sleep(1)
        stop_qemu(proc)
        proc = None

        proc, con, bootlog = boot_and_login(disks)
        check("boot 2: MBR probe line", "system disk (MBR" in bootlog)
        out = sh(con, "fatswap %s" % mid_path, 240)
        check("boot 2: swap works after reboot", "fatswap: OK" in out,
              out.strip()[:120])
        con.sendline("reboot")
        con.read_until(["AIOS reboot -- resetting board"], 60)
        time.sleep(1)
        stop_qemu(proc)
        proc = None
        v = Fat32(img)
        check("boot 2: final extraction matches", v.extract(KERNEL11) == mid_bytes)
        check("boot 2: FAT copies match", v.fats_equal())

        # ---- bare ext2 image regression (no MBR) ----
        print("=== boot 3: bare ext2 image (regression) ===", flush=True)
        bare_disks = [ext2_copy] + disks[1:]
        proc, con, bootlog = boot_and_login(bare_disks)
        check("bare: boots without MBR", "system disk" in bootlog)
        check("bare: no MBR misdetect", "system disk (MBR" not in bootlog)
        sh(con, "printf 'x' > /tmp/t", 15)
        out = sh(con, "fatswap /tmp/t", 30)
        check("bare: fatswap fails -2 (no boot partition)", "FAIL (-2)" in out,
              out.strip()[:80])
        con.sendline("reboot")
        con.read_until(["AIOS reboot -- resetting board"], 60)
        time.sleep(1)
        stop_qemu(proc)
        proc = None

    except Exception as e:
        import traceback
        traceback.print_exc()
        check("harness exception", False, "%s: %s" % (type(e).__name__, e))
    finally:
        if proc is not None:
            stop_qemu(proc)
        if os.path.exists(SOCK):
            os.unlink(SOCK)
        shutil.rmtree(workdir, ignore_errors=True)

    npass = sum(1 for _, ok in results if ok)
    print("\n=== fatswap QEMU test: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())

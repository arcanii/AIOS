# AIOS Handover Notes -- Claude Sonnet 4.6 to Claude Opus 4.6
# Session: 2026-04-15

This document records the state of the AIOS repository as of the end of the
Claude Sonnet 4.6 session. The successor model (Opus 4.6, 1M context) should
read this before taking any action.

---

## 1. Repository and Environment

- **Repo:** `/home/bryan/Source/github/AIOS`
- **Host OS:** Linux Ubuntu 25.10 (NOT macOS -- AI_BRIEFING.md is outdated)
- **Shell:** zsh
- **Cross-compiler:** `aarch64-linux-gnu-gcc` (via apt, not Homebrew)
- **Current version:** `v0.4.94` (`include/aios/version.h` patch = 94)
- **Branch:** `main` (no RPi4 branch -- PLAT_RPI4 cmake separation is sufficient)
- **Build dirs:** `build-04` (QEMU virt), `build-rpi4` (RPi4 BCM2711)

---

## 2. Context: What v0.4.93 Delivered

See `docs/NEXT_20260414c.md` for full details. Short summary:

- RPi4 hardware boots to interactive login
- HDMI via VideoCore mailbox + fb_info device-untyped trick
- SD card read-only driver (`blk_emmc.c`, CMD17 PIO)
- Mini UART input (AUX block 0xFE215000, NOT PL011)
- DTB parser fixes (32-bit vs 64-bit address-cells, VC-to-ARM translation)

---

## 3. v0.4.94 Work: What Was Done This Session

### 3.1 Version Bump

`include/aios/version.h`: patch bumped 93 -> 94. Committed.

### 3.2 deps/ Patches Applied (and verified)

These patches are required for RPi4 build. They are NOT committed to deps/
(deps are git submodules maintained separately). Apply them after any fresh
`git submodule update`.

| File | Change | Status |
|------|--------|--------|
| `deps/kernel/src/plat/bcm2711/overlay-rpi4.dts` | `reserved-memory@3b400000` -> `reserved-memory@3a000000`, reg `0x3b400000` -> `0x3a000000` | **APPLIED** |
| `deps/kernel/src/plat/bcm2711/overlay-rpi4-4gb.dts` | Memory reg `0x3b400000` -> `0x3a000000` | **APPLIED** |
| `deps/kernel/tools/dts/rpi4.dts` | `/memreserve/ 0x0 0x1000` -> `0x0 0x200000` (padded 64-bit format) | **APPLIED** |
| `deps/musllibc/src/internal/vis.h` | `visibility push(protected)` -> `visibility push(default)` | **APPLIED** |
| `deps/musllibc/src/internal/stdio_impl.h` | `visibility("protected")` -> `visibility("default")` (GCC 15 fix) | **APPLIED** |
| `deps/seL4_libs/libsel4platsupport/src/common.c` | Lines ~275-276 `seL4_DebugPutString` warning calls commented out | **APPLIED** |
| `deps/seL4_tools/elfloader-tool/src/common.c` | printf disable for BCM2711 | **NOT YET APPLIED** (see section 4) |

### 3.3 CMD24 Write Support -- DESIGNED, NOT YET WRITTEN

The `plat_blk_write()` function in `src/plat/rpi4/blk_emmc.c` is still the
Phase 1 stub (returns -1). The write implementation was fully designed but not
yet written to disk before context ran out.

**Design (implement this):**

Add `emmc_write_raw_sector()` after `emmc_read_raw_sector()` (~line 400):

```c
static int emmc_write_raw_sector(uint64_t lba, const void *buf) {
    static uint32_t sector_buf[128];  /* shared with read path */

    if (emmc_wait_dat() != 0) return -1;
    memcpy(sector_buf, buf, 512);

    EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);          /* clear pending */
    EMMC_W(REG_BLKSIZECNT, (1u << 16) | 0x200); /* 1 block, 512 bytes */

    uint32_t arg = card_is_sdhc ? (uint32_t)lba : (uint32_t)(lba * 512);
    EMMC_W(REG_ARGUMENT, arg);
    EMMC_W(REG_XFER_CMD,
        CMD_INDEX(24) | CMD_RESP_48 | CMD_CRC_EN | CMD_IDX_EN |
        CMD_DATA | XFER_BLKCNT_EN);
    /* note: NO XFER_READ flag -- write direction */

    /* Wait for CMD done */
    if (emmc_wait_int(INT_CMD_DONE, 100000) != 0) return -1;
    /* Check response (R1) for errors */
    uint32_t resp = EMMC_R(REG_RESPONSE0);
    if (resp & 0xFDF90008) return -1;

    /* Wait for write buffer ready */
    if (emmc_wait_int(INT_BUF_WR, 100000) != 0) return -1;

    /* Write 128 x 32-bit words */
    for (int i = 0; i < 128; i++)
        EMMC_W(REG_DATA, sector_buf[i]);

    /* Wait for transfer complete */
    if (emmc_wait_int(INT_XFER_DONE, 500000) != 0) return -1;

    /* Poll PRES_WRITE_ACTIVE until card finishes programming */
    for (int i = 0; i < 100000; i++) {
        if (!(EMMC_R(REG_PRES_STATE) & PRES_WRITE_ACTIVE))
            return 0;
        for (volatile int d = 0; d < 100; d++) {}
    }
    return -1; /* card busy timeout */
}
```

Then replace the stub in `plat_blk_write()`:

```c
int plat_blk_write(uint64_t sector, const void *buf) {
    if (!emmc_initialized) return -1;
    return emmc_write_raw_sector(part_offset + sector, buf);
}
```

Required register constants (check blk_emmc.c -- some may already exist):
- `INT_BUF_WR` -- write buffer available interrupt bit
- `PRES_WRITE_ACTIVE` -- present state: write transfer active
- `CMD_DATA` -- XFER_CMD bit: data transfer present
- `XFER_BLKCNT_EN` -- transfer mode: block count enable

### 3.4 Test Files -- NOT YET CREATED

**`src/apps/emmc_write_test.c`** -- POSIX test app:
- Open `/tmp/wtest.bin` for write, write 512-byte incrementing pattern (0x00..0xFF repeated)
- Close, reopen for read, memcmp verify
- Overwrite with inverted pattern, read-back verify
- unlink, verify open fails with ENOENT
- Print PASS/FAIL

**`disk/rootfs/bin/tests/test_094.sh`** -- shell regression test:
- Uses dash built-ins only (no bash)
- Write/read-back/append/overwrite via redirection + xxd/hexdump if available
- Invoke `/bin/aios/emmc_write_test` binary
- Print PASS/FAIL per subtest

**`projects/aios/CMakeLists.txt`** -- add `AiosPosixApp(emmc_write_test)` after
the `AiosPosixApp(writetest)` line.

---

## 4. Outstanding Issues

### 4.1 Elfloader printf Disable (MUST FIX before RPi4 build)

The elfloader uses mini UART (BCM2711 serial1) which hangs the CPU bus if
accessed from elfloader context. printf must be disabled.

**Current state:** `deps/seL4_tools/elfloader-tool/src/common.c` line 34 is
`extern char _bss[];` -- the printf suppression block is NOT present.

**Correct fix (do NOT use awk -- gawk on Ubuntu 25.10 treats # as comment
in print strings even inside double quotes):**

```sh
printf '#if defined(CONFIG_PLAT_BCM2711)\n#undef printf\n#define printf(...) ((void)0)\n#endif\n' > /tmp/insert_elfloader.c
sed -i '32r /tmp/insert_elfloader.c' deps/seL4_tools/elfloader-tool/src/common.c
```

Insert point is after line 32 (`#include <abort.h>`). The block should appear
between the last `#include` and the first `extern` declaration.

Verify:
```sh
grep -A3 'CONFIG_PLAT_BCM2711' deps/seL4_tools/elfloader-tool/src/common.c
```

### 4.2 Dash syntax.h Missing

`mksyntax` was run but `syntax.h` was not generated in `deps/dash/src/`.
`nodes.h`, `builtins.h`, `signames.c` are present.

Investigation needed:
```sh
cd deps/dash/src && ls -la syntax.h mksyntax* 2>/dev/null
cd deps/dash/src && ./mksyntax 2>&1 | head -20
```

`syntax.h` is generated by `mksyntax` from `mksyntax.c`. If the binary exists
but syntax.h is absent, the build step may have failed silently, or output went
to a different location.

### 4.3 tools/seL4 Is a Directory, Not a Symlink

An earlier verify check used `test -L tools/seL4` which fails. `tools/seL4`
is a git-tracked DIRECTORY (not a symlink). The correct structure is:

```
tools/seL4/cmake-tool -> ../deps/seL4_tools/cmake-tool  (symlink INSIDE)
```

The build path `tools/seL4/cmake-tool/all.cmake` resolves correctly.
The verify check was wrong -- the build structure is fine.

---

## 5. Scripting Constraints (Critical for Future Sessions)

These constraints were discovered during this session. Violating them causes
silent failures or terminal errors.

### 5.1 Single Code Block Rule

**ALL scripts must go in a single code block.** The user pastes entire responses
directly into the terminal. Any markdown text, bold formatting, or dashes
outside/between code blocks cause terminal errors.

Use `echo` banners inside the block to label sections:
```sh
echo "=== Script A: Patch elfloader ==="
# ... patch commands ...
echo "=== Script B: Verify ==="
# ... verify commands ...
```

### 5.2 No Heredocs

Heredocs (`<< EOF`) are unreliable when pasted into terminal -- the EOF
delimiter can get leading whitespace from indentation, or the `>` prompt
persists if the terminal doesn't see the delimiter on its own line.

**Use instead:**
- `printf '...\n'` with `\n` escapes for multi-line content
- `sed -i` for file edits
- `printf '...' > /tmp/file.c && sed -i 'Nr /tmp/file.c'` for inserting blocks

### 5.3 No `#` in awk Print Strings

gawk on Ubuntu 25.10 treats `#` as a comment character even inside
double-quoted strings inside `print "..."` statements. This causes
"runaway string constant" errors and can CORRUPT FILES (awk outputs nothing,
piped to file via `mv` overwrites the original with empty content).

**Do NOT use awk to insert C preprocessor directives.** Use `printf + sed -i 'r'`
as shown in section 4.1.

If awk corrupts a file, restore with: `cd deps/<module> && git checkout <path>`

---

## 6. Build Instructions (First RPi4 Build)

After applying the elfloader patch and fixing syntax.h:

```sh
cd /home/bryan/Source/github/AIOS
mkdir -p build-rpi4
cd build-rpi4
cmake -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- \
    -DAIOS_SETTINGS=../settings-rpi4.cmake \
    ..
ninja
```

If CMakeLists errors on `AiosPosixApp(emmc_write_test)` -- that line hasn't
been added yet (see section 3.4).

---

## 7. Next Session Priorities (from NEXT_20260414c.md)

1. **SD card write support** -- implement CMD24 per section 3.3 above
2. **emmc_write_test.c** -- POSIX write test app (section 3.4)
3. **test_094.sh** -- shell regression test (section 3.4)
4. **Full RPi4 build** -- after elfloader patch + syntax.h resolved
5. **USB HID keyboard** -- PCIe root complex + xHCI
6. **RPi4 GENET Ethernet** -- BCM54213 for SSH
7. **SMP** -- spin table wakeup for secondary cores

---

## 8. Key Architecture Reminders

- **PAL selection:** compile-time via `PLAT_RPI4` / `PLAT_QEMU_VIRT` cmake defines
- **blk_hal.h:** `plat_blk_read/write(sector, buf)` -- sector is EXT2-relative (partition offset added internally)
- **seL4 device untypeds:** NOT zeroed on retype -- enables fb_info trick at 0x3A000000
- **Mini UART:** AUX block 0xFE215000, NOT PL011 (0xFE201000 = Bluetooth)
- **BCM2711 address translation:** VC bus 0x7Exxxxxx -> ARM 0xFExxxxxx (+0x80000000)
- **SD card partitions:** P1=FAT32 (boot), P2=ext2 (AIOS system disk, type 0x83)
- **MBR parsing:** blk_emmc.c reads sector 0, finds type 0x83 partition, stores LBA offset

---

*Handover written by Claude Sonnet 4.6, session 2026-04-15*

/*
 * blk_emmc.c -- BCM2711 EMMC2 SDHCI block driver (RPi4)
 *
 * Phase 2: Read/write PIO mode. CMD17 reads, CMD24 writes.
 * Parses MBR to find ext2 partition, adds sector offset.
 *
 * BCM2711 EMMC2 is an Arasan SDHCI controller at ARM phys 0xFE340000.
 * The DTB reports VC bus address 0x7E340000 (untranslated).
 * All register access must be 32-bit (BCM2711 restriction).
 */
#include "aios/root_shared.h"
#include "aios/vka_audit.h"
#include <sel4platsupport/device.h>
#include "aios/device_map.h"
#include <stdio.h>
#include <string.h>
#include "arch.h"
#include "aios/hw_info.h"
#include "plat/blk_hal.h"


/* ----------------------------------------------------------------
 * SDHCI register offsets (all 32-bit access on BCM2711)
 * ---------------------------------------------------------------- */
#define REG_ARG2          0x00
#define REG_BLKSIZECNT    0x04
#define REG_ARGUMENT      0x08
#define REG_XFER_CMD      0x0C
#define REG_RESPONSE0     0x10
#define REG_RESPONSE1     0x14
#define REG_RESPONSE2     0x18
#define REG_RESPONSE3     0x1C
#define REG_BUFFER        0x20
#define REG_PRESENT       0x24
#define REG_HOST_CTRL     0x28
#define REG_CLOCK_CTRL    0x2C
#define REG_INT_STATUS    0x30
#define REG_INT_ENABLE    0x34
#define REG_INT_SIGNAL    0x38
#define REG_CAPABILITIES  0x40
#define REG_MAX_CURRENT   0x48
#define REG_SLOT_VERSION  0xFC

/* PRESENT_STATE bits */
#define PRES_CMD_INHIBIT  (1u << 0)
#define PRES_DAT_INHIBIT  (1u << 1)
#define PRES_WRITE_ACTIVE (1u << 8)
#define PRES_READ_ACTIVE  (1u << 9)
#define PRES_BUF_WR_EN    (1u << 10)
#define PRES_BUF_RD_EN    (1u << 11)
#define PRES_CARD_INSERT  (1u << 16)

/* CLOCK_CTRL bits (lower 16 of the 32-bit register) */
#define CLK_INT_EN        (1u << 0)
#define CLK_INT_STABLE    (1u << 1)
#define CLK_SD_EN         (1u << 2)

/* Software reset bits (byte 3 of CLOCK_CTRL register) */
#define RESET_ALL         (1u << 24)
#define RESET_CMD         (1u << 25)
#define RESET_DAT         (1u << 26)

/* INT_STATUS bits */
#define INT_CMD_DONE      (1u << 0)
#define INT_XFER_DONE     (1u << 1)
#define INT_BLK_GAP       (1u << 2)
#define INT_DMA           (1u << 3)
#define INT_BUF_WR        (1u << 4)
#define INT_BUF_RD        (1u << 5)
#define INT_ERROR         (1u << 15)
#define INT_ERR_CMD_TO    (1u << 16)
#define INT_ERR_CMD_CRC   (1u << 17)
#define INT_ERR_CMD_END   (1u << 18)
#define INT_ERR_CMD_IDX   (1u << 19)
#define INT_ERR_DAT_TO    (1u << 20)
#define INT_ERR_DAT_CRC   (1u << 21)
#define INT_ERR_DAT_END   (1u << 22)
#define INT_ALL_ERR       0xFFFF0000u

/* XFER_CMD encoding (32-bit write to 0x0C)
 * Bits 31:24 = command index
 * Bits 23:22 = command type (00 = normal)
 * Bit 21     = data present
 * Bit 20     = command index check
 * Bit 19     = command CRC check
 * Bits 17:16 = response type (00=none, 01=136, 10=48, 11=48busy)
 * Bits 5:4   = transfer mode: bit5=multi, bit4=read
 * Bit 1      = block count enable
 * Bit 0      = DMA enable */
#define CMD_INDEX(n)      ((uint32_t)(n) << 24)
#define CMD_RESP_NONE     (0u << 16)
#define CMD_RESP_136      (1u << 16)
#define CMD_RESP_48       (2u << 16)
#define CMD_RESP_48B      (3u << 16)
#define CMD_CRC_EN        (1u << 19)
#define CMD_IDX_EN        (1u << 20)
#define CMD_DATA          (1u << 21)
#define XFER_READ         (1u << 4)
#define XFER_MULTI        (1u << 5)
#define XFER_BLKCNT_EN    (1u << 1)

/* HOST_CTRL power control bits (byte 1 of register at 0x28) */
#define PWR_ON            (1u << 8)
#define PWR_3V3           (7u << 9)

/* ----------------------------------------------------------------
 * Static driver state
 * ---------------------------------------------------------------- */
static volatile uint32_t *emmc_regs;
static uint32_t card_rca;
static int card_is_sdhc;
static uint32_t part_offset;
static int emmc_initialized;

/* v0.4.222 fatswap: FAT32 boot partition (MBR type 0x0b/0x0c) location,
 * recorded during the MBR parse. 0 = none found. */
static uint64_t boot_part_start;
static uint64_t boot_part_sectors;

/* Aligned sector buffer for PIO reads (avoids alignment issues) */
static uint32_t __attribute__((aligned(16))) sector_buf[128];

/* ----------------------------------------------------------------
 * Register access macros
 * ---------------------------------------------------------------- */
#define EMMC_R(off)      (emmc_regs[(off) / 4])
#define EMMC_W(off, val) do { emmc_regs[(off) / 4] = (val); arch_dsb(); } while (0)

/* ----------------------------------------------------------------
 * emmc_delay -- crude microsecond delay (spin loop)
 * ---------------------------------------------------------------- */
static void emmc_delay(int us) {
    for (volatile int i = 0; i < us * 100; i++) {
        asm volatile("" ::: "memory");
    }
}

/* ----------------------------------------------------------------
 * Time-based waits (ARM generic timer) -- replace the old fixed
 * iteration-count poll loops. A fixed count of MMIO reads is NOT a
 * real timeout: on the 1.5GHz A72 a MISSED status bit (a known reality
 * of polled SDHCI) burned the whole 10,000,000-count loop ~= 32.6s,
 * while QEMU finished instantly -- the v0.4.175 netconsole2 relay stall.
 * A millisecond bound from cntpct_el0 is CPU-speed independent and caps
 * a missed bit at EMMC_*_MS instead of spinning for half a minute.
 * ---------------------------------------------------------------- */
#define EMMC_CMD_MS    1000   /* command response / line-free          */
#define EMMC_DATA_MS   2000   /* data phase (buffer ready / xfer done)  */

static inline uint64_t emmc_ticks(void) {
    uint64_t c; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(c)); return c;
}
static uint64_t emmc_deadline(int ms) {
    uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (!f) f = 54000000;   /* BCM2711 generic-timer fallback */
    return emmc_ticks() + (uint64_t)ms * f / 1000;
}
/* Poll INT_STATUS until `want` is latched, an error latches, or `ms` elapse.
 * Returns 1 = want seen, 0 = timed out, -1 = INT_ERROR latched (caller
 * re-reads REG_INT_STATUS for the detail, the bit stays latched until
 * cleared).
 *
 * v0.4.224: a data-phase TIMEOUT is a FAILURE, not a shrug. The old
 * "callers proceed" fall-through let a read drain an empty FIFO: under a
 * 32.4s-multiple stall quantum the 2s deadline expires while the CPU is
 * frozen, BUF_RD never latches, and REG_BUFFER returns 0xFFFFFFFF -- which
 * is exactly how a network push landed 124 bytes of 0xFF inside an ext2
 * block (the fatswap deploy corruption, 2026-06-12). Callers now treat 0 as
 * a hard error and retry the whole (idempotent) command once. */
static int emmc_wait_int(uint32_t want, int ms) {
    uint64_t dl = emmc_deadline(ms);
    for (;;) {
        arch_dmb();
        uint32_t st = EMMC_R(REG_INT_STATUS);
        if (st & INT_ERROR) return -1;
        if (st & want) return 1;
        if (emmc_ticks() >= dl) return 0;
    }
}

/* ----------------------------------------------------------------
 * emmc_wait_cmd -- wait for command line free
 * ---------------------------------------------------------------- */
static int emmc_wait_cmd(void) {
    uint64_t dl = emmc_deadline(EMMC_CMD_MS);
    do {
        arch_dmb();
        if (!(EMMC_R(REG_PRESENT) & PRES_CMD_INHIBIT))
            return 0;
    } while (emmc_ticks() < dl);
    printf("[blk] CMD inhibit timeout\n");
    return -1;
}

/* ----------------------------------------------------------------
 * emmc_wait_dat -- wait for data line free
 * ---------------------------------------------------------------- */
static int emmc_wait_dat(void) {
    uint64_t dl = emmc_deadline(EMMC_DATA_MS);
    do {
        arch_dmb();
        if (!(EMMC_R(REG_PRESENT) & PRES_DAT_INHIBIT))
            return 0;
    } while (emmc_ticks() < dl);
    printf("[blk] DAT inhibit timeout\n");
    return -1;
}

/* ----------------------------------------------------------------
 * emmc_send_cmd -- send an SDHCI command and wait for completion
 *
 * cmd:  combined XFER_CMD register value (index + flags)
 * arg:  command argument
 * Returns 0 on success, -1 on timeout/error
 * ---------------------------------------------------------------- */
static int emmc_send_cmd(uint32_t cmd, uint32_t arg) {
    if (emmc_wait_cmd() != 0) return -1;

    /* Clear all pending interrupts */
    EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);

    EMMC_W(REG_ARGUMENT, arg);
    EMMC_W(REG_XFER_CMD, cmd);

    /* Poll for command complete or error (time-bounded) */
    int r = emmc_wait_int(INT_CMD_DONE, EMMC_CMD_MS);
    if (r < 0) {
        printf("[blk] CMD%u error: INT=0x%x\n",
               (cmd >> 24) & 0x3F, EMMC_R(REG_INT_STATUS));
        EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
        return -1;
    }
    if (r == 0) {
        printf("[blk] CMD%u timeout\n", (cmd >> 24) & 0x3F);
        return -1;
    }
    EMMC_W(REG_INT_STATUS, INT_CMD_DONE);   /* clear command done */
    return 0;
}

/* ----------------------------------------------------------------
 * emmc_send_app_cmd -- send CMD55 followed by an app command
 * ---------------------------------------------------------------- */
static int emmc_send_app_cmd(uint32_t cmd, uint32_t arg) {
    /* CMD55 (APP_CMD): tell card next command is application-specific */
    int err = emmc_send_cmd(
        CMD_INDEX(55) | CMD_RESP_48 | CMD_CRC_EN | CMD_IDX_EN,
        card_rca << 16);
    if (err) {
        printf("[blk] CMD55 failed\n");
        return -1;
    }
    return emmc_send_cmd(cmd, arg);
}

/* ----------------------------------------------------------------
 * emmc_set_clock -- set SD clock frequency
 * base_khz: target frequency in kHz (e.g. 400, 25000)
 * ---------------------------------------------------------------- */
static void emmc_set_clock(uint32_t target_khz) {
    /* Read base clock from capabilities register (bits 15:8, in MHz) */
    arch_dmb();
    uint32_t caps = EMMC_R(REG_CAPABILITIES);
    uint32_t base_mhz = (caps >> 8) & 0xFF;
    if (base_mhz == 0) base_mhz = 200;  /* BCM2711 default */
    uint32_t base_khz = base_mhz * 1000;

    /* Disable SD clock */
    arch_dmb();
    uint32_t clk = EMMC_R(REG_CLOCK_CTRL);
    clk &= 0xFFFF0000u;  /* preserve reset bits in upper half */
    EMMC_W(REG_CLOCK_CTRL, clk);

    /* Calculate divider: freq = base / (2 * div)
     * SDHCI v3 10-bit divided clock mode */
    uint32_t div = base_khz / (2 * target_khz);
    if (div == 0) div = 1;
    if (base_khz / (2 * div) > target_khz) div++;
    if (div > 1023) div = 1023;

    /* Encode: bits 15:8 = lower 8 bits of div, bits 7:6 = upper 2 bits */
    uint32_t div_lo = div & 0xFF;
    uint32_t div_hi = (div >> 8) & 0x3;
    clk = (div_lo << 8) | (div_hi << 6) | CLK_INT_EN;
    EMMC_W(REG_CLOCK_CTRL, clk);

    /* Wait for internal clock stable */
    for (int t = 0; t < 100000; t++) {
        arch_dmb();
        if (EMMC_R(REG_CLOCK_CTRL) & CLK_INT_STABLE)
            break;
    }

    /* Enable SD clock */
    arch_dmb();
    clk = EMMC_R(REG_CLOCK_CTRL);
    clk |= CLK_SD_EN;
    EMMC_W(REG_CLOCK_CTRL, clk);
    emmc_delay(2000);  /* settle */

    printf("[blk] Clock: base=%uMHz div=%u target=%ukHz\n",
           base_mhz, div, base_khz / (2 * div));
}

/* ----------------------------------------------------------------
 * emmc_read_raw_sector -- read one 512-byte sector via PIO
 *
 * v0.4.224: completion TIMEOUTS now fail the attempt instead of falling
 * through. The fall-through drained an un-ready FIFO (returns 0xFFFFFFFF
 * per word) when a stall quantum expired the 2s deadline -- silent 0xFF
 * corruption in whatever sat above (the fatswap push corruption). The
 * public wrapper retries the whole command once (CMD17 is idempotent),
 * then fails LOUDLY. Counters surface in /proc/cachestats.
 * ---------------------------------------------------------------- */
extern volatile uint32_t blk_emmc_timeout_retries;   /* blk_cache.c */
extern volatile uint32_t blk_emmc_timeout_fails;     /* blk_cache.c */

/* One CMD17 attempt into sector_buf.
 * Returns 0 = ok, -1 = hard error, -2 = completion wait timed out. */
static int emmc_read_attempt(uint64_t lba) {
    if (emmc_wait_dat() != 0) return -1;

    /* Clear interrupts */
    EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);

    /* Block size = 512, block count = 1 */
    EMMC_W(REG_BLKSIZECNT, (1u << 16) | 0x200);

    /* CMD17: READ_SINGLE_BLOCK */
    uint32_t arg = card_is_sdhc ? (uint32_t)lba : (uint32_t)(lba * 512);
    EMMC_W(REG_ARGUMENT, arg);
    EMMC_W(REG_XFER_CMD,
        CMD_INDEX(17) | CMD_RESP_48 | CMD_CRC_EN | CMD_IDX_EN |
        CMD_DATA | XFER_READ | XFER_BLKCNT_EN);

    /* Wait for command complete */
    int r = emmc_wait_int(INT_CMD_DONE, EMMC_CMD_MS);
    if (r < 0) {
        printf("[blk] Read LBA %u cmd err: 0x%x\n", (uint32_t)lba, EMMC_R(REG_INT_STATUS));
        EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
        return -1;
    }
    if (r == 0) return -2;
    EMMC_W(REG_INT_STATUS, INT_CMD_DONE);

    /* Wait for buffer read ready. NEVER drain the FIFO on a timeout:
     * an un-ready data port reads back 0xFFFFFFFF. */
    r = emmc_wait_int(INT_BUF_RD, EMMC_DATA_MS);
    if (r < 0) {
        printf("[blk] Read LBA %u data err: 0x%x\n", (uint32_t)lba, EMMC_R(REG_INT_STATUS));
        EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
        return -1;
    }
    if (r == 0) return -2;
    EMMC_W(REG_INT_STATUS, INT_BUF_RD);

    /* Read 128 words (512 bytes) from data port */
    for (int i = 0; i < 128; i++) {
        arch_dmb();
        sector_buf[i] = EMMC_R(REG_BUFFER);
    }

    /* Wait for transfer complete. BUF_RD latched and the FIFO was
     * drained, so the data in hand is real; a missed XFER_DONE here is
     * bookkeeping -- accept the data (this was the legitimate half of
     * the old fall-through). Hard errors still fail. */
    r = emmc_wait_int(INT_XFER_DONE, EMMC_DATA_MS);
    if (r < 0) {
        printf("[blk] Read LBA %u xfer err: 0x%x\n", (uint32_t)lba, EMMC_R(REG_INT_STATUS));
        EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
        return -1;
    }
    EMMC_W(REG_INT_STATUS, INT_XFER_DONE);
    return 0;
}

static int emmc_read_raw_sector(uint64_t lba, void *buf) {
    int r = emmc_read_attempt(lba);
    if (r == -2) {
        blk_emmc_timeout_retries++;
        printf("[blk] Read LBA %u completion timeout -- retrying once\n",
               (uint32_t)lba);
        r = emmc_read_attempt(lba);
        if (r == -2) {
            blk_emmc_timeout_fails++;
            printf("[blk] Read LBA %u timed out twice -- FAILED\n",
                   (uint32_t)lba);
            return -1;
        }
    }
    if (r != 0) return -1;

    /* Copy from aligned buffer to caller */
    memcpy(buf, sector_buf, 512);
    return 0;
}

/* ----------------------------------------------------------------
 * emmc_read_multi -- read `count` contiguous 512-byte sectors via PIO using
 * CMD18 READ_MULTIPLE_BLOCK + CMD12 STOP. One command covers all blocks, far
 * fewer per-sector overheads than count separate CMD17 reads -- the read-side
 * mirror of the v0.4.172 CMD25 write speedup. Every wait is cntpct-bounded.
 * ---------------------------------------------------------------- */
/* One CMD18 attempt. Returns 0 = ok, -1 = hard error, -2 = a completion wait
 * timed out (CMD12 stop sent best-effort so the retry starts from a stopped
 * card). Mirror of emmc_write_multi_attempt with the read deltas. */
static int emmc_read_multi_attempt(uint64_t lba, void *buf, int count) {
    if (emmc_wait_dat() != 0) return -1;

    EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
    EMMC_W(REG_BLKSIZECNT, ((uint32_t)count << 16) | 0x200);
    uint32_t arg = card_is_sdhc ? (uint32_t)lba : (uint32_t)(lba * 512);
    EMMC_W(REG_ARGUMENT, arg);
    /* CMD18: multi-block read (XFER_READ + XFER_MULTI, block-count enabled) */
    EMMC_W(REG_XFER_CMD,
        CMD_INDEX(18) | CMD_RESP_48 | CMD_CRC_EN | CMD_IDX_EN |
        CMD_DATA | XFER_READ | XFER_MULTI | XFER_BLKCNT_EN);

    int r = emmc_wait_int(INT_CMD_DONE, EMMC_CMD_MS);
    if (r < 0) {
        printf("[blk] ReadM LBA %u cmd err: 0x%x\n", (uint32_t)lba, EMMC_R(REG_INT_STATUS));
        EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
        return -1;
    }
    if (r == 0) return -2;
    EMMC_W(REG_INT_STATUS, INT_CMD_DONE);

    /* Drain each block: the controller signals buffer-read-ready one block at
     * a time, so re-wait INT_BUF_RD per block, then pull 128 words. Never drain
     * an un-ready FIFO (it reads back 0xFFFFFFFF) -- a timeout fails the run. */
    uint8_t *p = (uint8_t *)buf;
    for (int b = 0; b < count; b++) {
        r = emmc_wait_int(INT_BUF_RD, EMMC_DATA_MS);
        if (r < 0) {
            printf("[blk] ReadM LBA %u buf err: 0x%x\n", (uint32_t)lba, EMMC_R(REG_INT_STATUS));
            EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
            return -1;
        }
        if (r == 0) {
            emmc_send_cmd(CMD_INDEX(12) | CMD_RESP_48B | CMD_CRC_EN | CMD_IDX_EN, 0);
            return -2;
        }
        EMMC_W(REG_INT_STATUS, INT_BUF_RD);
        for (int i = 0; i < 128; i++) {
            arch_dmb();
            sector_buf[i] = EMMC_R(REG_BUFFER);
        }
        memcpy(p + b * 512, sector_buf, 512);
    }

    /* Wait for transfer complete; on timeout stop the card before a retry. */
    r = emmc_wait_int(INT_XFER_DONE, EMMC_DATA_MS);
    if (r < 0) {
        printf("[blk] ReadM LBA %u xfer err: 0x%x\n", (uint32_t)lba, EMMC_R(REG_INT_STATUS));
        EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
        return -1;
    }
    if (r == 0) {
        emmc_send_cmd(CMD_INDEX(12) | CMD_RESP_48B | CMD_CRC_EN | CMD_IDX_EN, 0);
        return -2;
    }
    EMMC_W(REG_INT_STATUS, INT_XFER_DONE);

    /* CMD12 STOP_TRANSMISSION ends the open-ended multi-block read. */
    if (emmc_send_cmd(CMD_INDEX(12) | CMD_RESP_48B | CMD_CRC_EN | CMD_IDX_EN, 0) != 0)
        return -1;
    return 0;
}

static int emmc_read_multi(uint64_t lba, void *buf, int count) {
    if (count <= 0) return 0;
    if (count == 1) return emmc_read_raw_sector(lba, buf);
    int r = emmc_read_multi_attempt(lba, buf, count);
    if (r == -2) {
        blk_emmc_timeout_retries++;
        printf("[blk] ReadM LBA %u completion timeout -- retrying once\n", (uint32_t)lba);
        r = emmc_read_multi_attempt(lba, buf, count);
        if (r == -2) {
            blk_emmc_timeout_fails++;
            printf("[blk] ReadM LBA %u timed out twice -- FAILED\n", (uint32_t)lba);
            return -1;
        }
    }
    return r == 0 ? 0 : -1;
}

/* ----------------------------------------------------------------
 * plat_blk_init -- Initialize BCM2711 EMMC2 and detect SD card
 * ---------------------------------------------------------------- */
int plat_blk_init(void) {
    int err;

    if (!hw_info.has_emmc) {
        printf("[blk] No eMMC in DTB\n");
        return -1;
    }

    /* Address translation: VC bus -> ARM physical */
    /* hw_info.emmc_paddr is already ARM physical (DTB parser translates) */
    uint64_t arm_paddr = hw_info.emmc_paddr;

    printf("[blk] eMMC DTB=0x%lx ARM=0x%lx IRQ=%u\n",
           (unsigned long)hw_info.emmc_paddr,
           (unsigned long)arm_paddr,
           hw_info.emmc_irq);

    /* v0.4.149: use the pre-mapped eMMC MMIO (ascending-order device map). */
    (void)arm_paddr;
    if (!dev_emmc_vaddr) {
        printf("[blk] eMMC MMIO not pre-mapped\n");
        return -1;
    }
    emmc_regs = dev_emmc_vaddr;

    /* Verify controller is alive by reading version register */
    arch_dmb();
    uint32_t ver = EMMC_R(REG_SLOT_VERSION);
    if (ver == 0xFFFFFFFF || ver == 0) {
        printf("[blk] SDHCI not responding (ver=0x%x, clock off?)\n", ver);
        return -1;
    }
    printf("[blk] SDHCI version: 0x%x (spec %u.0)\n",
           ver, ((ver >> 16) & 0xFF) + 1);

    /* --- Controller reset --- */
    EMMC_W(REG_CLOCK_CTRL, RESET_ALL);
    for (int t = 0; t < 1000000; t++) {
        arch_dmb();
        if (!(EMMC_R(REG_CLOCK_CTRL) & RESET_ALL))
            break;
    }
    emmc_delay(5000);

    /* Enable all interrupt status flags (polling, no signal delivery) */
    EMMC_W(REG_INT_ENABLE, 0x01FF01FFu);
    EMMC_W(REG_INT_SIGNAL, 0);
    EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);

    /* Set timeout to maximum */
    arch_dmb();
    uint32_t clk_ctrl = EMMC_R(REG_CLOCK_CTRL);
    clk_ctrl = (clk_ctrl & ~(0xFu << 16)) | (0xEu << 16);
    EMMC_W(REG_CLOCK_CTRL, clk_ctrl);

    /* Set power: 3.3V, power on */
    arch_dmb();
    uint32_t host = EMMC_R(REG_HOST_CTRL);
    host = (host & ~0xFF00u) | PWR_ON | PWR_3V3;
    EMMC_W(REG_HOST_CTRL, host);
    emmc_delay(5000);

    /* Set identification clock (~400kHz) */
    emmc_set_clock(400);

    /* --- SD card initialization --- */

    /* CMD0: GO_IDLE_STATE */
    emmc_send_cmd(CMD_INDEX(0) | CMD_RESP_NONE, 0);
    emmc_delay(10000);

    /* CMD8: SEND_IF_COND (SDv2 detection) */
    card_is_sdhc = 0;
    err = emmc_send_cmd(
        CMD_INDEX(8) | CMD_RESP_48 | CMD_CRC_EN | CMD_IDX_EN,
        0x000001AA);
    if (err == 0) {
        arch_dmb();
        uint32_t r8 = EMMC_R(REG_RESPONSE0);
        if ((r8 & 0xFFF) != 0x1AA) {
            printf("[blk] CMD8 echo mismatch: 0x%x\n", r8);
            return -1;
        }
        printf("[blk] CMD8 OK (SDv2)\n");
    } else {
        printf("[blk] CMD8 failed (SDv1 card?)\n");
    }

    /* ACMD41: SD_SEND_OP_COND (loop until ready) */
    uint32_t acmd41_arg = 0x40100000u;  /* HCS=1, 3.2-3.4V */
    int card_ready = 0;
    for (int retry = 0; retry < 100; retry++) {
        /* ACMD41 response is R3 (no CRC, no index check) */
        err = emmc_send_app_cmd(
            CMD_INDEX(41) | CMD_RESP_48,
            acmd41_arg);
        if (err) {
            emmc_delay(10000);
            continue;
        }
        arch_dmb();
        uint32_t ocr = EMMC_R(REG_RESPONSE0);
        if (ocr & 0x80000000u) {
            card_ready = 1;
            card_is_sdhc = (ocr & 0x40000000u) ? 1 : 0;
            printf("[blk] ACMD41 ready: OCR=0x%x SDHC=%d\n",
                   ocr, card_is_sdhc);
            break;
        }
        emmc_delay(10000);
    }
    if (!card_ready) {
        printf("[blk] ACMD41 timeout (no card?)\n");
        return -1;
    }

    /* CMD2: ALL_SEND_CID (136-bit response) */
    err = emmc_send_cmd(CMD_INDEX(2) | CMD_RESP_136 | CMD_CRC_EN, 0);
    if (err) {
        printf("[blk] CMD2 failed\n");
        return -1;
    }
    printf("[blk] CMD2 OK (CID read)\n");

    /* CMD3: SEND_RELATIVE_ADDR */
    err = emmc_send_cmd(
        CMD_INDEX(3) | CMD_RESP_48 | CMD_CRC_EN | CMD_IDX_EN, 0);
    if (err) {
        printf("[blk] CMD3 failed\n");
        return -1;
    }
    arch_dmb();
    card_rca = (EMMC_R(REG_RESPONSE0) >> 16) & 0xFFFF;
    printf("[blk] CMD3 OK: RCA=0x%x\n", card_rca);

    /* CMD7: SELECT_CARD (enter transfer state) */
    err = emmc_send_cmd(
        CMD_INDEX(7) | CMD_RESP_48B | CMD_CRC_EN | CMD_IDX_EN,
        card_rca << 16);
    if (err) {
        printf("[blk] CMD7 failed\n");
        return -1;
    }
    printf("[blk] CMD7 OK (card selected)\n");

    /* Switch to data transfer clock (~25MHz) */
    emmc_set_clock(25000);

    /* ACMD6: SET_BUS_WIDTH (4-bit) */
    err = emmc_send_app_cmd(
        CMD_INDEX(6) | CMD_RESP_48 | CMD_CRC_EN | CMD_IDX_EN,
        0x00000002);  /* arg 2 = 4-bit bus */
    if (err == 0) {
        arch_dmb();
        host = EMMC_R(REG_HOST_CTRL);
        host |= (1u << 1);  /* bit 1 = 4-bit data width */
        EMMC_W(REG_HOST_CTRL, host);
        printf("[blk] 4-bit bus enabled\n");
    }

    /* --- Read MBR and find ext2 partition --- */
    uint8_t mbr[512];
    if (emmc_read_raw_sector(0, mbr) != 0) {
        printf("[blk] MBR read failed\n");
        return -1;
    }

    /* Check MBR signature */
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        printf("[blk] Bad MBR signature: 0x%02x 0x%02x\n",
               mbr[510], mbr[511]);
        return -1;
    }

    /* Search partition table for Linux ext2 (type 0x83) */
    part_offset = 0;
    int found = 0;
    for (int p = 0; p < 4; p++) {
        uint8_t *e = &mbr[0x1BE + p * 16];
        uint8_t type = e[4];
        uint32_t lba = (uint32_t)e[8]  | ((uint32_t)e[9] << 8) |
                       ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t cnt = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                       ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        if (type != 0)
            printf("[blk] MBR part%d: type=0x%02x LBA=%u size=%uMB\n",
                   p + 1, type, lba, cnt / 2048);
        if (type == 0x83 && !found) {
            part_offset = lba;
            found = 1;
        }
        /* v0.4.222 fatswap: remember the FAT boot partition */
        if ((type == 0x0B || type == 0x0C) && boot_part_start == 0) {
            boot_part_start = lba;
            boot_part_sectors = cnt;
        }
    }

    if (!found) {
        printf("[blk] No ext2 partition (type 0x83) in MBR\n");
        return -1;
    }

    printf("[blk] ext2 partition at sector %u\n", part_offset);

    /* Verify ext2 superblock at partition offset + sector 2 */
    uint8_t sb[512];
    if (emmc_read_raw_sector(part_offset + 2, sb) != 0) {
        printf("[blk] Superblock read failed\n");
        return -1;
    }
    uint16_t magic = (uint16_t)sb[0x38] | ((uint16_t)sb[0x39] << 8);
    if (magic != 0xEF53) {
        printf("[blk] Bad ext2 magic: 0x%04x (expected 0xEF53)\n", magic);
        return -1;
    }

    emmc_initialized = 1;
    printf("[blk] RPi4 SD card ready (SDHC=%d, part=%u, ext2 OK)\n",
           card_is_sdhc, part_offset);
    return 0;
}

/* ----------------------------------------------------------------
 * HAL: plat_blk_read -- read sector relative to ext2 partition
 * ---------------------------------------------------------------- */
int plat_blk_read(uint64_t sector, void *buf) {
    if (!emmc_initialized) return -1;
    return emmc_read_raw_sector(part_offset + sector, buf);
}

/* ----------------------------------------------------------------
 * emmc_write_raw_sector -- write one 512-byte sector via PIO
 *
 * v0.4.224: same hardening as the read path. A write whose XFER_DONE
 * never latched may be UNPROGRAMMED -- reporting success there loses the
 * write while the cache above clears its dirty bit. Timeouts fail the
 * attempt; the wrapper retries once (rewriting the same data to the same
 * sector is idempotent), then fails loudly.
 * ---------------------------------------------------------------- */
/* One CMD24 attempt. Returns 0 = ok, -1 = hard error, -2 = timeout. */
static int emmc_write_attempt(uint64_t lba, const void *buf) {
    if (emmc_wait_dat() != 0) return -1;

    /* Clear interrupts */
    EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);

    /* Block size = 512, block count = 1 */
    EMMC_W(REG_BLKSIZECNT, (1u << 16) | 0x200);

    /* CMD24: WRITE_SINGLE_BLOCK */
    uint32_t arg = card_is_sdhc ? (uint32_t)lba : (uint32_t)(lba * 512);
    EMMC_W(REG_ARGUMENT, arg);
    EMMC_W(REG_XFER_CMD,
        CMD_INDEX(24) | CMD_RESP_48 | CMD_CRC_EN | CMD_IDX_EN |
        CMD_DATA | XFER_BLKCNT_EN);

    /* Wait for command complete */
    int r = emmc_wait_int(INT_CMD_DONE, EMMC_CMD_MS);
    if (r < 0) {
        printf("[blk] Write LBA %u cmd err: 0x%x\n", (uint32_t)lba, EMMC_R(REG_INT_STATUS));
        EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
        return -1;
    }
    if (r == 0) return -2;
    EMMC_W(REG_INT_STATUS, INT_CMD_DONE);

    /* Wait for buffer write ready */
    r = emmc_wait_int(INT_BUF_WR, EMMC_DATA_MS);
    if (r < 0) {
        printf("[blk] Write LBA %u buf err: 0x%x\n", (uint32_t)lba, EMMC_R(REG_INT_STATUS));
        EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
        return -1;
    }
    if (r == 0) return -2;
    EMMC_W(REG_INT_STATUS, INT_BUF_WR);

    /* Copy caller data into aligned buffer then write 128 words */
    memcpy(sector_buf, buf, 512);
    for (int i = 0; i < 128; i++) {
        EMMC_W(REG_BUFFER, sector_buf[i]);
    }

    /* Wait for transfer complete -- on a WRITE this is the card accepting
     * the data; without it the block may never be programmed. */
    r = emmc_wait_int(INT_XFER_DONE, EMMC_DATA_MS);
    if (r < 0) {
        printf("[blk] Write LBA %u xfer err: 0x%x\n", (uint32_t)lba, EMMC_R(REG_INT_STATUS));
        EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
        return -1;
    }
    if (r == 0) return -2;
    EMMC_W(REG_INT_STATUS, INT_XFER_DONE);

    return 0;
}

static int emmc_write_raw_sector(uint64_t lba, const void *buf) {
    int r = emmc_write_attempt(lba, buf);
    if (r == -2) {
        blk_emmc_timeout_retries++;
        printf("[blk] Write LBA %u completion timeout -- retrying once\n",
               (uint32_t)lba);
        r = emmc_write_attempt(lba, buf);
        if (r == -2) {
            blk_emmc_timeout_fails++;
            printf("[blk] Write LBA %u timed out twice -- FAILED\n",
                   (uint32_t)lba);
            return -1;
        }
    }
    return r == 0 ? 0 : -1;
}

/* ----------------------------------------------------------------
 * emmc_write_multi -- write `count` contiguous 512-byte sectors via PIO
 * using CMD25 WRITE_MULTIPLE_BLOCK + CMD12 STOP_TRANSMISSION. One flash
 * program cycle covers all blocks, so this is far faster per sector than
 * count separate CMD24 single-block writes (the v0.4.172 write speedup).
 * Returns 0 on success, -1 on error.
 * ---------------------------------------------------------------- */
/* One CMD25 attempt. Returns 0 = ok, -1 = hard error, -2 = a completion
 * wait timed out (CMD12 stop sent best-effort so the retry starts from a
 * stopped card). v0.4.224 hardening -- see emmc_write_raw_sector. */
static int emmc_write_multi_attempt(uint64_t lba, const void *buf, int count) {
    if (emmc_wait_dat() != 0) return -1;

    EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
    EMMC_W(REG_BLKSIZECNT, ((uint32_t)count << 16) | 0x200);
    uint32_t arg = card_is_sdhc ? (uint32_t)lba : (uint32_t)(lba * 512);
    EMMC_W(REG_ARGUMENT, arg);
    /* CMD25: multi-block write (XFER_MULTI, block-count enabled, no READ bit) */
    EMMC_W(REG_XFER_CMD,
        CMD_INDEX(25) | CMD_RESP_48 | CMD_CRC_EN | CMD_IDX_EN |
        CMD_DATA | XFER_MULTI | XFER_BLKCNT_EN);

    /* Wait for command complete */
    int r = emmc_wait_int(INT_CMD_DONE, EMMC_CMD_MS);
    if (r < 0) {
        printf("[blk] WriteM LBA %u cmd err: 0x%x\n", (uint32_t)lba, EMMC_R(REG_INT_STATUS));
        EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
        return -1;
    }
    if (r == 0) return -2;
    EMMC_W(REG_INT_STATUS, INT_CMD_DONE);

    /* Transfer each block: wait for buffer-write-ready, push 128 words */
    const uint8_t *p = (const uint8_t *)buf;
    for (int b = 0; b < count; b++) {
        r = emmc_wait_int(INT_BUF_WR, EMMC_DATA_MS);
        if (r < 0) {
            printf("[blk] WriteM LBA %u buf err: 0x%x\n", (uint32_t)lba, EMMC_R(REG_INT_STATUS));
            EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
            return -1;
        }
        if (r == 0) {
            /* Mid-stream timeout: stop the open-ended write so the card
             * leaves data state, then let the wrapper retry the run. */
            emmc_send_cmd(CMD_INDEX(12) | CMD_RESP_48B | CMD_CRC_EN
                          | CMD_IDX_EN, 0);
            return -2;
        }
        EMMC_W(REG_INT_STATUS, INT_BUF_WR);
        memcpy(sector_buf, p + b * 512, 512);
        for (int i = 0; i < 128; i++) {
            EMMC_W(REG_BUFFER, sector_buf[i]);
        }
    }

    /* Wait for transfer complete (all blocks accepted) -- without it the
     * tail of the run may never be programmed. */
    r = emmc_wait_int(INT_XFER_DONE, EMMC_DATA_MS);
    if (r < 0) {
        printf("[blk] WriteM LBA %u xfer err: 0x%x\n", (uint32_t)lba, EMMC_R(REG_INT_STATUS));
        EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
        return -1;
    }
    if (r == 0) {
        emmc_send_cmd(CMD_INDEX(12) | CMD_RESP_48B | CMD_CRC_EN
                      | CMD_IDX_EN, 0);
        return -2;
    }
    EMMC_W(REG_INT_STATUS, INT_XFER_DONE);

    /* CMD12 STOP_TRANSMISSION ends the open-ended multi-block write
     * (AUTO_CMD12 is not enabled in our XFER encoding). */
    if (emmc_send_cmd(CMD_INDEX(12) | CMD_RESP_48B | CMD_CRC_EN | CMD_IDX_EN, 0)
        != 0)
        return -1;

    return 0;
}

static int emmc_write_multi(uint64_t lba, const void *buf, int count) {
    if (count <= 0) return 0;
    if (count == 1) return emmc_write_raw_sector(lba, buf);
    int r = emmc_write_multi_attempt(lba, buf, count);
    if (r == -2) {
        blk_emmc_timeout_retries++;
        printf("[blk] WriteM LBA %u completion timeout -- retrying once\n",
               (uint32_t)lba);
        r = emmc_write_multi_attempt(lba, buf, count);
        if (r == -2) {
            blk_emmc_timeout_fails++;
            printf("[blk] WriteM LBA %u timed out twice -- FAILED\n",
                   (uint32_t)lba);
            return -1;
        }
    }
    return r == 0 ? 0 : -1;
}

/* ----------------------------------------------------------------
 * HAL: plat_blk_write -- write sector relative to ext2 partition
 * ---------------------------------------------------------------- */
int plat_blk_write(uint64_t sector, const void *buf) {
    if (!emmc_initialized) return -1;
    return emmc_write_raw_sector(part_offset + sector, buf);
}

/* HAL: plat_blk_write_multi -- write `count` contiguous sectors at once. */
int plat_blk_write_multi(uint64_t sector, const void *buf, int count) {
    if (!emmc_initialized) return -1;
    return emmc_write_multi(part_offset + sector, buf, count);
}

/* HAL: plat_blk_read_multi -- read `count` contiguous sectors at once (CMD18). */
int plat_blk_read_multi(uint64_t sector, void *buf, int count) {
    if (!emmc_initialized) return -1;
    return emmc_read_multi(part_offset + sector, buf, count);
}

/* ----------------------------------------------------------------
 * HAL: absolute-LBA I/O (v0.4.222 fatswap) -- whole-card sector
 * space, no partition offset, no block cache. fs_thread only (the
 * PIO sector_buf is shared with the cache-backend paths above).
 * ---------------------------------------------------------------- */
int plat_blk_read_abs(uint64_t sector, void *buf) {
    if (!emmc_initialized) return -1;
    return emmc_read_raw_sector(sector, buf);
}

int plat_blk_write_abs(uint64_t sector, const void *buf) {
    if (!emmc_initialized) return -1;
    return emmc_write_raw_sector(sector, buf);
}

int plat_blk_write_multi_abs(uint64_t sector, const void *buf, int count) {
    if (!emmc_initialized) return -1;
    return emmc_write_multi(sector, buf, count);
}

int plat_blk_read_multi_abs(uint64_t sector, void *buf, int count) {
    if (!emmc_initialized) return -1;
    return emmc_read_multi(sector, buf, count);
}

uint64_t plat_blk_boot_part_start(void)   { return boot_part_start; }
uint64_t plat_blk_boot_part_sectors(void) { return boot_part_sectors; }

/* ----------------------------------------------------------------
 * HAL: log drive stubs (RPi4 has single SD card, no log drive)
 * ---------------------------------------------------------------- */
int plat_blk_init_log(void) { return -1; }

int plat_blk_read_log(uint64_t sector, void *buf) {
    (void)sector; (void)buf;
    return -1;
}

int plat_blk_write_log(uint64_t sector, const void *buf) {
    (void)sector; (void)buf;
    return -1;
}

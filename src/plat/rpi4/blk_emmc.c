/*
 * blk_emmc.c -- BCM2711 EMMC2 SDHCI block driver (RPi4)
 *
 * Phase 1: Read-only PIO mode. Single-block CMD17 reads.
 * Parses MBR to find ext2 partition, adds sector offset.
 *
 * BCM2711 EMMC2 is an Arasan SDHCI controller at ARM phys 0xFE340000.
 * The DTB reports VC bus address 0x7E340000 (untranslated).
 * All register access must be 32-bit (BCM2711 restriction).
 */
#include "aios/root_shared.h"
#include "aios/vka_audit.h"
#include <sel4platsupport/device.h>
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
 * emmc_wait_cmd -- wait for command line free
 * ---------------------------------------------------------------- */
static int emmc_wait_cmd(void) {
    for (int t = 0; t < 1000000; t++) {
        arch_dmb();
        if (!(EMMC_R(REG_PRESENT) & PRES_CMD_INHIBIT))
            return 0;
    }
    printf("[blk] CMD inhibit timeout\n");
    return -1;
}

/* ----------------------------------------------------------------
 * emmc_wait_dat -- wait for data line free
 * ---------------------------------------------------------------- */
static int emmc_wait_dat(void) {
    for (int t = 0; t < 1000000; t++) {
        arch_dmb();
        if (!(EMMC_R(REG_PRESENT) & PRES_DAT_INHIBIT))
            return 0;
    }
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

    /* Poll for command complete or error */
    for (int t = 0; t < 10000000; t++) {
        arch_dmb();
        uint32_t st = EMMC_R(REG_INT_STATUS);
        if (st & INT_ERROR) {
            printf("[blk] CMD%u error: INT=0x%x\n",
                   (cmd >> 24) & 0x3F, st);
            EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
            return -1;
        }
        if (st & INT_CMD_DONE) {
            /* Clear command done bit */
            EMMC_W(REG_INT_STATUS, INT_CMD_DONE);
            return 0;
        }
    }
    printf("[blk] CMD%u timeout\n", (cmd >> 24) & 0x3F);
    return -1;
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
 * lba:  physical sector number on SD card
 * buf:  destination buffer (512 bytes)
 * Returns 0 on success, -1 on error
 * ---------------------------------------------------------------- */
static int emmc_read_raw_sector(uint64_t lba, void *buf) {
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
    for (int t = 0; t < 10000000; t++) {
        arch_dmb();
        uint32_t st = EMMC_R(REG_INT_STATUS);
        if (st & INT_ERROR) {
            printf("[blk] Read LBA %u cmd err: 0x%x\n", (uint32_t)lba, st);
            EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
            return -1;
        }
        if (st & INT_CMD_DONE) break;
    }
    EMMC_W(REG_INT_STATUS, INT_CMD_DONE);

    /* Wait for buffer read ready */
    for (int t = 0; t < 10000000; t++) {
        arch_dmb();
        uint32_t st = EMMC_R(REG_INT_STATUS);
        if (st & INT_ERROR) {
            printf("[blk] Read LBA %u data err: 0x%x\n", (uint32_t)lba, st);
            EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
            return -1;
        }
        if (st & INT_BUF_RD) break;
    }
    EMMC_W(REG_INT_STATUS, INT_BUF_RD);

    /* Read 128 words (512 bytes) from data port */
    for (int i = 0; i < 128; i++) {
        arch_dmb();
        sector_buf[i] = EMMC_R(REG_BUFFER);
    }

    /* Wait for transfer complete */
    for (int t = 0; t < 10000000; t++) {
        arch_dmb();
        uint32_t st = EMMC_R(REG_INT_STATUS);
        if (st & INT_ERROR) {
            printf("[blk] Read LBA %u xfer err: 0x%x\n", (uint32_t)lba, st);
            EMMC_W(REG_INT_STATUS, 0xFFFFFFFF);
            return -1;
        }
        if (st & INT_XFER_DONE) break;
    }
    EMMC_W(REG_INT_STATUS, INT_XFER_DONE);

    /* Copy from aligned buffer to caller */
    memcpy(buf, sector_buf, 512);
    return 0;
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

    /* Map SDHCI register page */
    vka_object_t emmc_frame;
    err = sel4platsupport_alloc_frame_at(&vka, arm_paddr,
                                          seL4_PageBits, &emmc_frame);
    if (err) {
        /* Fallback: try raw VC address (in case seL4 translated it) */
        printf("[blk] ARM addr alloc failed (%d), trying VC addr\n", err);
        err = sel4platsupport_alloc_frame_at(&vka, hw_info.emmc_paddr,
                                              seL4_PageBits, &emmc_frame);
        if (err) {
            printf("[blk] MMIO alloc failed: %d\n", err);
            return -1;
        }
        printf("[blk] Using VC address for MMIO\n");
    }

    void *emmc_vaddr = vspace_map_pages(&vspace, &emmc_frame.cptr, NULL,
        seL4_AllRights, 1, seL4_PageBits, 0);
    if (!emmc_vaddr) {
        printf("[blk] MMIO map failed\n");
        return -1;
    }

    /* Register base (page-aligned, no offset needed for 0xFE340000) */
    emmc_regs = (volatile uint32_t *)emmc_vaddr;

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
 * HAL: plat_blk_write -- Phase 1 stub (read-only)
 * ---------------------------------------------------------------- */
int plat_blk_write(uint64_t sector, const void *buf) {
    (void)sector; (void)buf;
    return -1;
}

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

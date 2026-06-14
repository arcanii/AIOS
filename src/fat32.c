/*
 * fat32.c -- v0.4.222 flash-over-network: rewrite KERNEL8.IMG on the FAT32
 * boot partition in place, so a kernel deploy is a network push + reboot
 * instead of a physical card swap.
 *
 * This is NOT a filesystem mount: it is a single-purpose, crash-ordered
 * rewrite of one existing root-directory file's contents. The RPi4 firmware
 * is intolerant of FAT relayout (memory: mtools-formatted partitions do not
 * boot), so we never reformat, never move or create directory entries, and
 * never touch any other file. The only things written are: free data
 * clusters, FAT entries, the existing 32-byte dirent, and FSInfo.
 *
 * THREADING: must run on the fs_thread (the FS_FATSWAP handler). The source
 * file is read through the ext2 context (block cache) and the FAT partition
 * is written through the absolute-LBA HAL (plat_blk_*_abs); both the cache
 * and the backend DMA/PIO state are fs_thread-owned single-owner structures
 * (blk_cache.c, flush_server.c). The absolute LBA space used here is
 * physically disjoint from the cached ext2-relative space, so bypassing the
 * cache is coherent by construction.
 *
 * CRASH-SAFETY ORDER (a power cut at any point leaves a bootable kernel):
 *   1. allocate a NEW chain from free clusters (old chain untouched)
 *   2. write all file data into the new clusters
 *   3. link the new chain in every FAT copy
 *   4. COMMIT: rewrite the 32-byte dirent -- one single-sector write
 *   5. free the old chain in every FAT copy, update FSInfo
 * Before step 4 the dirent still references the intact old kernel (worst
 * case: the new clusters leak as a lost chain, which fsck reclaims). After
 * step 4 the new kernel is live (worst case: the old chain leaks).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "aios/root_shared.h"
#include "aios/fat32.h"
#include "plat/blk_hal.h"
#define LOG_MODULE "fat32"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "aios/aios_log.h"

/* ---------------------------------------------------------------- */
/* sha256 -- compact standalone implementation (FIPS 180-4). Used    */
/* for the deploy-verification habit: the tool reports the hash of   */
/* what it wrote AND what reads back through the new dirent.         */
/* ---------------------------------------------------------------- */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    int      fill;
} sha256_t;

static const uint32_t sha_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_t *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + sha_k[i] + w[i];
        uint32_t s0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(sha256_t *s) {
    s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85;
    s->h[2] = 0x3c6ef372; s->h[3] = 0xa54ff53a;
    s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c;
    s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
    s->len = 0; s->fill = 0;
}

static void sha256_update(sha256_t *s, const void *data, uint32_t n) {
    const uint8_t *p = (const uint8_t *)data;
    s->len += n;
    while (n > 0) {
        uint32_t take = 64 - (uint32_t)s->fill;
        if (take > n) take = n;
        memcpy(s->buf + s->fill, p, take);
        s->fill += (int)take;
        p += take; n -= take;
        if (s->fill == 64) { sha256_block(s, s->buf); s->fill = 0; }
    }
}

static void sha256_final(sha256_t *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    pad = 0;
    while (s->fill != 56) sha256_update(s, &pad, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - i * 8));
    sha256_update(s, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(s->h[i] >> 24);
        out[i*4+1] = (uint8_t)(s->h[i] >> 16);
        out[i*4+2] = (uint8_t)(s->h[i] >> 8);
        out[i*4+3] = (uint8_t)(s->h[i]);
    }
}

/* ---------------------------------------------------------------- */
/* Geometry + working state                                          */
/* ---------------------------------------------------------------- */

/* Chain-table capacity: 32768 clusters covers a 16 MB kernel at the
 * smallest cluster size (512 B). kernel8.img is ~1.6 MB today. */
#define FAT32_MAX_CHAIN   32768
#define FAT32_CHUNK_BYTES 65536   /* data streaming chunk (128 sectors) */

#define FAT_EOC      0x0FFFFFFFu
#define FAT_EOC_MIN  0x0FFFFFF8u
#define FAT_MASK     0x0FFFFFFFu

typedef struct {
    uint64_t boot_lba;       /* partition start (absolute)            */
    uint64_t fat_lba;        /* first FAT copy (absolute)             */
    uint64_t data_lba;       /* cluster 2 (absolute)                  */
    uint32_t fatsz;          /* sectors per FAT copy                  */
    uint32_t nfats;
    uint32_t spc;            /* sectors per cluster                   */
    uint32_t cluster_bytes;
    uint32_t cluster_count;  /* number of data clusters               */
    uint32_t root_cluster;
    uint32_t fsinfo_sector;  /* partition-relative; 0 = none          */
} fat_geom_t;

static fat_geom_t g;

static uint8_t  chunk_buf[FAT32_CHUNK_BYTES];
static uint8_t  sec_buf[512];        /* BPB / dirent / FSInfo / FAT RMW  */
static uint8_t  fat_cache[512];      /* 1-sector cache for chain walks   */
static uint64_t fat_cache_lba;       /* 0 = invalid                      */
static uint32_t new_chain[FAT32_MAX_CHAIN];
static uint32_t old_chain[FAT32_MAX_CHAIN];

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int cluster_valid(uint32_t cl) {
    return cl >= 2 && cl < g.cluster_count + 2;
}
static uint64_t cluster_lba(uint32_t cl) {
    return g.data_lba + (uint64_t)(cl - 2) * g.spc;
}

/* Read the FAT entry for `cl` from FAT copy 0 via a 1-sector cache.
 * Chains and free-scans are mostly sequential, so this is ~1 read per
 * 128 entries. Returns 0 and sets *val (masked to 28 bits). */
static int fat_entry(uint32_t cl, uint32_t *val) {
    uint64_t lba = g.fat_lba + (cl * 4) / 512;
    if (lba != fat_cache_lba) {
        if (plat_blk_read_abs(lba, fat_cache) != 0) return FAT32_ERR_IO;
        fat_cache_lba = lba;
    }
    *val = le32(fat_cache + (cl * 4) % 512) & FAT_MASK;
    return 0;
}

/* ---------------------------------------------------------------- */
/* BPB parse                                                         */
/* ---------------------------------------------------------------- */
static int parse_bpb(void) {
    g.boot_lba = plat_blk_boot_part_start();
    if (g.boot_lba == 0) {
        AIOS_LOG_WARN("no FAT boot partition on this disk");
        return FAT32_ERR_NOBOOT;
    }
    if (plat_blk_read_abs(g.boot_lba, sec_buf) != 0) return FAT32_ERR_IO;

    if (sec_buf[510] != 0x55 || sec_buf[511] != 0xAA) {
        AIOS_LOG_WARN("BPB signature missing");
        return FAT32_ERR_BPB;
    }
    uint32_t bps      = le16(sec_buf + 11);
    uint32_t spc      = sec_buf[13];
    uint32_t reserved = le16(sec_buf + 14);
    uint32_t nfats    = sec_buf[16];
    uint32_t fatsz16  = le16(sec_buf + 22);
    uint32_t totsec   = le32(sec_buf + 32);
    uint32_t fatsz32  = le32(sec_buf + 36);
    uint32_t rootcl   = le32(sec_buf + 44);
    uint32_t fsinfo   = le16(sec_buf + 48);

    if (totsec == 0) totsec = le16(sec_buf + 19);

    if (bps != 512 || spc == 0 || spc > 128 || (spc & (spc - 1)) != 0 ||
        nfats == 0 || nfats > 2 || fatsz16 != 0 || fatsz32 == 0 ||
        reserved == 0 || rootcl < 2 || totsec == 0) {
        AIOS_LOG_WARN_V("unsupported BPB, bytes/sector=", (unsigned long)bps);
        return FAT32_ERR_BPB;
    }

    uint64_t part_sectors = plat_blk_boot_part_sectors();
    if (part_sectors && totsec > part_sectors)
        AIOS_LOG_WARN_V("BPB totsec exceeds MBR partition size, totsec=",
                        (unsigned long)totsec);

    g.spc           = spc;
    g.cluster_bytes = spc * 512;
    g.nfats         = nfats;
    g.fatsz         = fatsz32;
    g.fat_lba       = g.boot_lba + reserved;
    g.data_lba      = g.fat_lba + (uint64_t)nfats * fatsz32;
    g.cluster_count = (uint32_t)((totsec - reserved - nfats * fatsz32) / spc);
    g.root_cluster  = rootcl;
    g.fsinfo_sector = fsinfo;
    fat_cache_lba   = 0;

    /* Each FAT copy must actually cover cluster_count entries. */
    if ((uint64_t)(g.cluster_count + 2) * 4 > (uint64_t)fatsz32 * 512) {
        AIOS_LOG_WARN("FAT smaller than cluster count");
        return FAT32_ERR_BPB;
    }

    AIOS_LOG_INFO_V("FAT32 at lba=", (unsigned long)g.boot_lba);
    AIOS_LOG_INFO_V("  clusters=", (unsigned long)g.cluster_count);
    AIOS_LOG_INFO_V("  cluster_bytes=", (unsigned long)g.cluster_bytes);
    return 0;
}

/* ---------------------------------------------------------------- */
/* Root-directory dirent locate (8.3 "KERNEL8 IMG", LFNs skipped)    */
/* ---------------------------------------------------------------- */
typedef struct {
    uint64_t lba;        /* absolute sector containing the dirent */
    uint32_t off;        /* byte offset of the entry in that sector */
    uint32_t first;      /* current first cluster (0 = empty file) */
    uint32_t size;
} dirent_loc_t;

/* Convert a friendly name ("config.txt") to the 11-byte 8.3 dirent field
 * ("CONFIG  TXT"). Returns 0, or -1 if not 8.3-representable (>8 name / >3 ext).
 * config.txt and kernel8.img are both 8.3-clean, so their dirents match directly
 * (we never create entries, so LFN-only names are out of scope by design). */
static int name_to_83(const char *name, char out[11]) {
    for (int k = 0; k < 11; k++) out[k] = ' ';
    int i = 0, o = 0;
    while (name[i] && name[i] != '.') {
        if (o >= 8) return -1;
        char c = name[i++];
        out[o++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    if (name[i] == '.') {
        i++; o = 8;
        while (name[i]) {
            if (o >= 11) return -1;
            char c = name[i++];
            out[o++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        }
    }
    return 0;
}

/* Locate a root-directory dirent by its 11-byte 8.3 name (LFNs skipped). */
static int find_dirent(const char target[11], dirent_loc_t *loc) {
    uint32_t cl = g.root_cluster;
    uint32_t hops = 0;

    while (1) {
        if (!cluster_valid(cl) || ++hops > g.cluster_count)
            return FAT32_ERR_CHAIN;
        uint64_t lba = cluster_lba(cl);
        for (uint32_t s = 0; s < g.spc; s++) {
            if (plat_blk_read_abs(lba + s, sec_buf) != 0) return FAT32_ERR_IO;
            for (uint32_t o = 0; o < 512; o += 32) {
                uint8_t first = sec_buf[o];
                uint8_t attr  = sec_buf[o + 11];
                if (first == 0x00) return FAT32_ERR_NODIRENT; /* end of dir */
                if (first == 0xE5) continue;                  /* deleted    */
                if ((attr & 0x0F) == 0x0F) continue;          /* LFN entry  */
                if (attr & 0x18) continue;                    /* volume/dir */
                if (memcmp(sec_buf + o, target, 11) != 0) continue;
                loc->lba   = lba + s;
                loc->off   = o;
                loc->first = ((uint32_t)le16(sec_buf + o + 20) << 16)
                           | le16(sec_buf + o + 26);
                loc->size  = le32(sec_buf + o + 28);
                return 0;
            }
        }
        uint32_t next;
        int rc = fat_entry(cl, &next);
        if (rc) return rc;
        if (next >= FAT_EOC_MIN) return FAT32_ERR_NODIRENT;
        cl = next;
    }
}

/* Collect a file's cluster chain into out[] (capacity FAT32_MAX_CHAIN).
 * Returns the count, or a negative error on loops/corruption. */
static int collect_chain(uint32_t first, uint32_t *out) {
    int n = 0;
    uint32_t cl = first;
    if (cl == 0) return 0;                      /* empty file */
    while (1) {
        if (!cluster_valid(cl)) return FAT32_ERR_CHAIN;
        if (n >= FAT32_MAX_CHAIN) return FAT32_ERR_TOOBIG;
        out[n++] = cl;
        uint32_t next;
        int rc = fat_entry(cl, &next);
        if (rc) return rc;
        if (next >= FAT_EOC_MIN) return n;
        if (next == 0 || next == 0x0FFFFFF7u) return FAT32_ERR_CHAIN;
        cl = next;
    }
}

/* Scan the FAT for `need` free clusters (ascending). The old chain's
 * entries are non-zero, so a free scan can never collide with it. */
static int alloc_chain(uint32_t need, uint32_t *out) {
    uint32_t got = 0;
    for (uint32_t cl = 2; cl < g.cluster_count + 2 && got < need; cl++) {
        uint32_t v;
        int rc = fat_entry(cl, &v);
        if (rc) return rc;
        if (v == 0) out[got++] = cl;
    }
    if (got < need) {
        AIOS_LOG_WARN_V("not enough free clusters, need=", (unsigned long)need);
        return FAT32_ERR_SPACE;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* FAT writes: sector-grouped read-modify-write over an ASCENDING    */
/* cluster list, mirrored to every FAT copy. set_links=1 writes the  */
/* chain links (list[i] -> list[i+1], EOC last); set_links=0 frees   */
/* (writes 0). The reserved top 4 bits of each entry are preserved.  */
/* ---------------------------------------------------------------- */
static int fat_write_entries(const uint32_t *list, int n, int set_links) {
    for (uint32_t f = 0; f < g.nfats; f++) {
        uint64_t base = g.fat_lba + (uint64_t)f * g.fatsz;
        uint64_t cur = 0;
        for (int i = 0; i < n; i++) {
            uint32_t cl = list[i];
            uint64_t lba = base + (cl * 4) / 512;
            if (lba != cur) {
                if (cur && plat_blk_write_abs(cur, sec_buf) != 0)
                    return FAT32_ERR_IO;
                if (plat_blk_read_abs(lba, sec_buf) != 0) return FAT32_ERR_IO;
                cur = lba;
            }
            uint8_t *e = sec_buf + (cl * 4) % 512;
            uint32_t val = 0;
            if (set_links)
                val = (i + 1 < n) ? list[i + 1] : FAT_EOC;
            put_le32(e, (le32(e) & ~FAT_MASK) | (val & FAT_MASK));
        }
        if (cur && plat_blk_write_abs(cur, sec_buf) != 0) return FAT32_ERR_IO;
    }
    fat_cache_lba = 0;   /* invalidate the read cache */
    return 0;
}

/* ---------------------------------------------------------------- */
/* Dirent commit: patch first-cluster/size/mtime in the existing     */
/* 32-byte entry. ONE single-sector write -- the atomic commit point. */
/* ---------------------------------------------------------------- */
static void fat_timestamp(uint16_t *tm, uint16_t *dt) {
    /* Civil date from epoch days (Howard Hinnant's algorithm). */
    long secs = aios_wall_now();
    long days = secs / 86400, rem = secs % 86400;
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - era * 146097);
    unsigned long yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned long doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned long mp = (5*doy + 2) / 153;
    unsigned long d = doy - (153*mp + 2)/5 + 1;
    unsigned long m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    long hh = rem / 3600, mm = (rem % 3600) / 60, ss = rem % 60;
    if (y < 1980) { y = 1980; m = 1; d = 1; hh = mm = ss = 0; }
    *tm = (uint16_t)((hh << 11) | (mm << 5) | (ss / 2));
    *dt = (uint16_t)(((y - 1980) << 9) | (m << 5) | d);
}

static int commit_dirent(const dirent_loc_t *loc, uint32_t first,
                         uint32_t size) {
    if (plat_blk_read_abs(loc->lba, sec_buf) != 0) return FAT32_ERR_IO;
    uint8_t *e = sec_buf + loc->off;
    uint16_t tm, dt;
    fat_timestamp(&tm, &dt);
    put_le16(e + 20, (uint16_t)(first >> 16));
    put_le16(e + 22, tm);
    put_le16(e + 24, dt);
    put_le16(e + 26, (uint16_t)first);
    put_le32(e + 28, size);
    if (plat_blk_write_abs(loc->lba, sec_buf) != 0) return FAT32_ERR_IO;
    return 0;
}

/* FSInfo: recompute the free count (one FAT pass) + next-free hint.
 * Advisory only -- skipped silently if the signatures are absent. */
static void update_fsinfo(uint32_t next_free_hint) {
    if (g.fsinfo_sector == 0) return;
    uint64_t lba = g.boot_lba + g.fsinfo_sector;
    if (plat_blk_read_abs(lba, sec_buf) != 0) return;
    if (le32(sec_buf) != 0x41615252u || le32(sec_buf + 484) != 0x61417272u)
        return;
    uint32_t free_count = 0;
    fat_cache_lba = 0;
    for (uint32_t cl = 2; cl < g.cluster_count + 2; cl++) {
        uint32_t v;
        if (fat_entry(cl, &v) != 0) return;
        if (v == 0) free_count++;
    }
    put_le32(sec_buf + 488, free_count);
    put_le32(sec_buf + 492, next_free_hint);
    plat_blk_write_abs(lba, sec_buf);
    AIOS_LOG_INFO_V("FSInfo free clusters=", (unsigned long)free_count);
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Hash `size` bytes read back through the chain starting at `first`. */
static int hash_chain(uint32_t first, uint32_t size, uint8_t out[32]) {
    sha256_t ctx;
    sha256_init(&ctx);
    uint32_t cl = first, left = size, hops = 0;
    fat_cache_lba = 0;
    while (left > 0) {
        if (!cluster_valid(cl) || ++hops > g.cluster_count)
            return FAT32_ERR_CHAIN;
        uint64_t lba = cluster_lba(cl);
        for (uint32_t s = 0; s < g.spc && left > 0; s++) {
            if (plat_blk_read_abs(lba + s, chunk_buf) != 0)
                return FAT32_ERR_IO;
            uint32_t take = left > 512 ? 512 : left;
            sha256_update(&ctx, chunk_buf, take);
            left -= take;
        }
        if (left > 0) {
            uint32_t next;
            int rc = fat_entry(cl, &next);
            if (rc) return rc;
            if (next >= FAT_EOC_MIN) return FAT32_ERR_CHAIN; /* short chain */
            cl = next;
        }
    }
    sha256_final(&ctx, out);
    return 0;
}

/* ---------------------------------------------------------------- */
/* fat32_swap_kernel -- the orchestration                            */
/* ---------------------------------------------------------------- */
int fat32_write_file(ext2_ctx_t *src_fs, const char *src_path,
                     const char *target_name, uint32_t abort_phase,
                     fat32_swap_result_t *out)
{
    memset(out, 0, sizeof(*out));

    char t83[11];
    if (name_to_83(target_name, t83) != 0) {
        AIOS_LOG_WARN("target name not 8.3-representable");
        return FAT32_ERR_NAME;
    }

    int rc = parse_bpb();
    if (rc) return rc;

    /* Source file on the system ext2 disk */
    uint32_t ino;
    if (ext2_resolve_path(src_fs, src_path, &ino) != 0) {
        AIOS_LOG_WARN("source file not found");
        return FAT32_ERR_SRC;
    }
    struct ext2_inode inode;
    if (ext2_read_inode(src_fs, ino, &inode) != 0) return FAT32_ERR_SRC;
    uint32_t size = inode.i_size;
    if (size == 0) return FAT32_ERR_SRC;

    uint32_t need = (size + g.cluster_bytes - 1) / g.cluster_bytes;
    if (need > FAT32_MAX_CHAIN) return FAT32_ERR_TOOBIG;

    /* Existing dirent + old chain */
    dirent_loc_t de;
    rc = find_dirent(t83, &de);
    if (rc) return rc;
    int n_old = collect_chain(de.first, old_chain);
    if (n_old < 0) return n_old;
    AIOS_LOG_INFO_V("old kernel bytes=", (unsigned long)de.size);
    AIOS_LOG_INFO_V("new kernel bytes=", (unsigned long)size);

    /* Phase 1: allocate the new chain (free entries only -- the live old
     * chain has non-zero entries, so it cannot be picked). */
    rc = alloc_chain(need, new_chain);
    if (rc) return rc;

    /* Phase 2: stream file data into the new clusters. Consecutive
     * clusters coalesce into multi-sector writes (CMD25 on the Pi). */
    sha256_t ctx;
    sha256_init(&ctx);
    uint32_t left = size, foff = 0, i = 0;
    while (left > 0) {
        uint32_t j = i + 1;
        while (j < need && new_chain[j] == new_chain[j - 1] + 1) j++;
        uint64_t lba = cluster_lba(new_chain[i]);
        uint64_t run_bytes = (uint64_t)(j - i) * g.cluster_bytes;
        if (run_bytes > left) run_bytes = left;
        while (run_bytes > 0) {
            uint32_t chunk = run_bytes > FAT32_CHUNK_BYTES
                           ? FAT32_CHUNK_BYTES : (uint32_t)run_bytes;
            int got = ext2_pread_file(src_fs, ino, (int)foff,
                                      (char *)chunk_buf, (int)chunk);
            if (got != (int)chunk) {
                AIOS_LOG_WARN_V("source read failed at off=",
                                (unsigned long)foff);
                return FAT32_ERR_SRC;
            }
            sha256_update(&ctx, chunk_buf, chunk);
            uint32_t secs = (chunk + 511) / 512;
            if (chunk % 512)
                memset(chunk_buf + chunk, 0, secs * 512 - chunk);
            if (plat_blk_write_multi_abs(lba, chunk_buf, (int)secs) != 0)
                return FAT32_ERR_IO;
            lba += secs;
            foff += chunk;
            left -= chunk;
            run_bytes -= chunk;
        }
        i = j;
    }
    sha256_final(&ctx, out->sha_src);
    out->bytes = size;
    out->clusters = need;
    out->first_cluster = new_chain[0];
    AIOS_LOG_INFO_V("data written, clusters=", (unsigned long)need);
    if (abort_phase == FAT32_ABORT_AFTER_DATA) {
        AIOS_LOG_WARN("DEBUG ABORT after data write");
        return 0;
    }

    /* Phase 3: link the new chain in every FAT copy. */
    rc = fat_write_entries(new_chain, (int)need, 1);
    if (rc) return rc;
    if (abort_phase == FAT32_ABORT_AFTER_FAT) {
        AIOS_LOG_WARN("DEBUG ABORT after FAT link");
        return 0;
    }

    /* Phase 4: COMMIT -- one single-sector dirent write. */
    rc = commit_dirent(&de, new_chain[0], size);
    if (rc) return rc;
    AIOS_LOG_INFO_V("dirent committed, first cluster=",
                    (unsigned long)new_chain[0]);
    if (abort_phase == FAT32_ABORT_AFTER_DIRENT) {
        AIOS_LOG_WARN("DEBUG ABORT after dirent commit");
        return 0;
    }

    /* Phase 5: free the old chain (sorted for sector-grouped RMW). */
    if (n_old > 0) {
        qsort(old_chain, (size_t)n_old, sizeof(uint32_t), cmp_u32);
        rc = fat_write_entries(old_chain, n_old, 0);
        if (rc) return rc;
    }
    update_fsinfo(new_chain[need - 1] + 1);

    /* Verify: re-read the dirent, hash the chain it references. */
    dirent_loc_t check;
    rc = find_dirent(t83, &check);
    if (rc) return rc;
    rc = hash_chain(check.first, check.size, out->sha_disk);
    if (rc) return rc;
    if (check.size != size || check.first != new_chain[0] ||
        memcmp(out->sha_src, out->sha_disk, 32) != 0) {
        AIOS_LOG_WARN("readback verification FAILED");
        return FAT32_ERR_VERIFY;
    }
    AIOS_LOG_INFO("swap complete, readback verified");
    return 0;
}

/* ---------------------------------------------------------------- */
/* fat32_read_file -- copy an existing root-dir file off the FAT     */
/* into dst (up to max bytes). Read-only: walks the dirent + chain,  */
/* never writes. Used to pull config.txt for an edit over the net.   */
/* ---------------------------------------------------------------- */
int fat32_read_file(const char *target_name, uint8_t *dst, uint32_t max,
                    uint32_t *out_size)
{
    char t83[11];
    if (name_to_83(target_name, t83) != 0) return FAT32_ERR_NAME;

    int rc = parse_bpb();
    if (rc) return rc;

    dirent_loc_t de;
    rc = find_dirent(t83, &de);
    if (rc) return rc;
    if (de.size > max) return FAT32_ERR_TOOBIG;

    uint32_t cl = de.first, off = 0;
    while (off < de.size) {
        if (!cluster_valid(cl)) return FAT32_ERR_CHAIN;
        uint64_t lba = cluster_lba(cl);
        for (uint32_t s = 0; s < g.spc && off < de.size; s++) {
            if (plat_blk_read_abs(lba + s, sec_buf) != 0) return FAT32_ERR_IO;
            uint32_t take = de.size - off;
            if (take > 512) take = 512;
            memcpy(dst + off, sec_buf, take);
            off += take;
        }
        uint32_t next;
        rc = fat_entry(cl, &next);
        if (rc) return rc;
        if (next >= FAT_EOC_MIN) break;
        cl = next;
    }
    *out_size = de.size;
    AIOS_LOG_INFO_V("fat read bytes=", (unsigned long)de.size);
    return 0;
}

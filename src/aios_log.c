/*
 * AIOS Log -- ring buffer + serial echo + file log
 *
 * Root-side threads share the ring buffer (same VSpace).
 * /proc/log reads the ring; serial gets real-time echo.
 * When a log drive is mounted at /log, entries are also
 * appended to /log/aios.log via direct ext2 writes.
 */
#include "aios/aios_log.h"
#include "aios/ext2.h"
#include <stdio.h>
#include <stdint.h>

/* ext2 context for the log drive (defined in aios_root.c) */
extern ext2_ctx_t ext2_log;

/* ---- Ring buffer ---- */

static char aios_log_ring[AIOS_LOG_RING_SIZE];
static int  aios_log_head = 0;
static int  aios_log_count = 0;

void aios_log_init(void) {
    aios_log_head = 0;
    aios_log_count = 0;
}

static void ring_putc(char c) {
    aios_log_ring[aios_log_head] = c;
    aios_log_head = (aios_log_head + 1) % AIOS_LOG_RING_SIZE;
    if (aios_log_count < AIOS_LOG_RING_SIZE) aios_log_count++;
}

int aios_log_read(char *buf, int bufsize) {
    if (aios_log_count == 0) return 0;
    int start;
    if (aios_log_count < AIOS_LOG_RING_SIZE)
        start = 0;
    else
        start = aios_log_head;
    int n = aios_log_count < bufsize - 1 ? aios_log_count : bufsize - 1;
    for (int i = 0; i < n; i++) {
        buf[i] = aios_log_ring[(start + i) % AIOS_LOG_RING_SIZE];
    }
    buf[n] = '\0';
    return n;
}

int aios_log_len(void) {
    return aios_log_count;
}

/* ---- File log ---- */

static int log_file_ready = 0;
static int log_file_pos = 0;
static uint32_t log_file_ino = 0;
static volatile int log_file_busy = 0;

static void log_file_flush_ring(void);

void aios_log_file_init(void) {
    if (log_file_ready) return;
    if (!ext2_log.read_sector) return;

    /* Check if aios.log already exists */
    uint32_t ino;
    if (ext2_resolve_path(&ext2_log, "aios.log", &ino) == 0) {
        struct ext2_inode inode;
        if (ext2_read_inode(&ext2_log, ino, &inode) == 0) {
            log_file_ino = ino;
            log_file_pos = (int)inode.i_size;
            log_file_ready = 1;
            printf("[log] Opened /log/aios.log (pos=%d)\n", log_file_pos);
            log_file_flush_ring();
            return;
        }
    }

    /* Create new log file with header */
    const char *hdr = "=== AIOS log ===\n";
    int hlen = 17;
    int ret = ext2_create_file(&ext2_log, 2, "aios.log", hdr, hlen);
    if (ret > 0) {
        log_file_ino = (uint32_t)ret;
        log_file_pos = hlen;
        log_file_ready = 1;
        printf("[log] Created /log/aios.log\n");
        log_file_flush_ring();
    } else {
        printf("[log] Failed to create aios.log: %d\n", ret);
    }
}

static void log_file_append(const char *buf, int len) {
    if (!log_file_ready || log_file_busy) return;
    log_file_busy = 1;
    int wrote = ext2_pwrite_file(&ext2_log, log_file_ino, log_file_pos,
                                 (const uint8_t *)buf, len);
    if (wrote > 0) log_file_pos += wrote;
    log_file_busy = 0;
}

/* Flush ring buffer to log file (captures boot entries) */
static void log_file_flush_ring(void) {
    if (!log_file_ready || aios_log_count == 0) return;
    int start;
    if (aios_log_count < AIOS_LOG_RING_SIZE)
        start = 0;
    else
        start = aios_log_head;
    int total = aios_log_count;
    int done = 0;
    char chunk[512];
    while (done < total) {
        int clen = total - done;
        if (clen > 512) clen = 512;
        for (int i = 0; i < clen; i++)
            chunk[i] = aios_log_ring[(start + done + i) % AIOS_LOG_RING_SIZE];
        int wrote = ext2_pwrite_file(&ext2_log, log_file_ino, log_file_pos,
                                     (const uint8_t *)chunk, clen);
        if (wrote <= 0) break;
        log_file_pos += wrote;
        done += clen;
    }
}

/* ---- Formatting ---- */

static const char *level_tag(int level) {
    switch (level) {
    case LOG_LEVEL_ERROR: return "ERR";
    case LOG_LEVEL_WARN:  return "WRN";
    case LOG_LEVEL_INFO:  return "INF";
    case LOG_LEVEL_DEBUG: return "DBG";
    default:              return "???";
    }
}

/* Format log line into buf. Returns length written. */
static int fmt_line(char *buf, int bufsz, int level, const char *module,
                    const char *msg, int has_val, unsigned long val) {
    int p = 0;

    /* Timestamp [HH:MM:SS] */
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    unsigned long ts = (unsigned long)(cnt / freq);
    unsigned long h = (ts / 3600) % 24;
    unsigned long m = (ts / 60) % 60;
    unsigned long s = ts % 60;
    buf[p++] = '[';
    buf[p++] = '0' + (h / 10); buf[p++] = '0' + (h % 10);
    buf[p++] = ':';
    buf[p++] = '0' + (m / 10); buf[p++] = '0' + (m % 10);
    buf[p++] = ':';
    buf[p++] = '0' + (s / 10); buf[p++] = '0' + (s % 10);
    buf[p++] = ']'; buf[p++] = ' ';

    /* Level tag */
    const char *t = level_tag(level);
    while (*t && p < bufsz - 30) buf[p++] = *t++;
    buf[p++] = ' ';

    /* Module */
    while (*module && p < bufsz - 30) buf[p++] = *module++;
    buf[p++] = ':'; buf[p++] = ' ';

    /* Message */
    while (*msg && p < bufsz - 25) buf[p++] = *msg++;

    /* Optional unsigned value */
    if (has_val) {
        char tmp[20];
        int ti = 0;
        if (val == 0) {
            buf[p++] = '0';
        } else {
            unsigned long v = val;
            while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; }
            while (ti-- > 0 && p < bufsz - 2) buf[p++] = tmp[ti];
        }
    }

    buf[p++] = '\n';
    buf[p] = '\0';
    return p;
}

/* ---- Public API ---- */

void aios_log_write(int level, const char *module, const char *msg) {
    char line[256];
    int len = fmt_line(line, sizeof(line), level, module, msg, 0, 0);

    /* Ring buffer */
    for (int i = 0; i < len; i++) ring_putc(line[i]);

    /* Serial echo */
    printf("[%s] %s: %s\n", level_tag(level), module, msg);

    /* File log */
    log_file_append(line, len);
}

void aios_log_write_v(int level, const char *module, const char *msg,
                   unsigned long val) {
    char line[256];
    int len = fmt_line(line, sizeof(line), level, module, msg, 1, val);

    /* Ring buffer */
    for (int i = 0; i < len; i++) ring_putc(line[i]);

    /* Serial echo */
    printf("[%s] %s: %s%lu\n", level_tag(level), module, msg, val);

    /* File log */
    log_file_append(line, len);
}

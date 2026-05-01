/*
 * boot_warmup.c -- v0.4.114 boot-time disk pre-load
 *
 * Read a hardcoded list of frequently-used files at boot, after the
 * filesystem is mounted but before login is shown. Reads go through
 * VFS -> ext2 -> blk_cache, so the side effect is a warmed cache:
 * the first interactive `ls`, `cat`, etc. hit the cache instead of
 * the disk.
 *
 * The hot-list is tuned from /proc/filehits captures of a basic
 * interactive session (login + a few minutes of ls/cat/grep/edit).
 * Order is informational; reads are sequential.
 *
 * AIOS root vfs_read returns the WHOLE file in one call into the
 * caller's buffer. We use an 8 KB buffer and ignore the data; the
 * cache fill is the entire point.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "aios/vfs.h"
#include "aios/blk_cache.h"
#define LOG_MODULE "warmup"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "aios/aios_log.h"

/* v0.4.116: tuned from /proc/filehits.
 *  + /etc/environment (~66 opens/session, every shell startup reads it)
 *  + /etc/motd        (~6 opens, login banner plus user reads)
 *  - /bin/aios/login  (binary does not exist; auth is in-root via auth_server)
 */
static const char *warm_files[] = {
    "/bin/dash",
    "/bin/zsh",
    "/bin/aios/getty",
    "/bin/ls",
    "/bin/cat",
    "/bin/echo",
    "/bin/wc",
    "/bin/head",
    "/bin/tail",
    "/bin/grep",
    "/bin/sed",
    "/bin/cp",
    "/bin/mv",
    "/bin/rm",
    "/bin/mkdir",
    "/etc/passwd",
    "/etc/hostname",
    "/etc/environment",
    "/etc/motd",
    NULL,
};

void boot_warmup_prefetch(void) {
    char tmp[8192];
    uint64_t total_bytes = 0;
    int file_hits = 0;
    int file_misses = 0;

    blk_cache_stats_t before, after;
    blk_cache_stats(&before);

    for (const char **p = warm_files; *p; p++) {
        int n = vfs_read(*p, tmp, sizeof(tmp));
        if (n > 0) {
            total_bytes += (uint64_t)n;
            file_hits++;
        } else {
            file_misses++;
        }
    }

    blk_cache_stats(&after);
    AIOS_LOG_INFO_V("files_loaded=", (unsigned long)file_hits);
    AIOS_LOG_INFO_V("files_missing=", (unsigned long)file_misses);
    AIOS_LOG_INFO_V("bytes_prefetched=", (unsigned long)total_bytes);
    AIOS_LOG_INFO_V("cache_pages_now=", (unsigned long)after.pages);
    AIOS_LOG_INFO_V("disk_misses_added=",
                    (unsigned long)(after.misses - before.misses));
}

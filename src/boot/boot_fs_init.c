/*
 * boot_fs_init.c -- Filesystem initialization (platform-agnostic)
 *
 * Calls platform HAL to init block device, then mounts ext2 + procfs.
 * All platform-specific code moved to src/plat/qemu-virt/blk_virtio.c.
 */
#include "aios/root_shared.h"
#include "aios/ext2.h"
#include "aios/vfs.h"
#include "aios/procfs.h"
#include "aios/blk_cache.h"
#define LOG_MODULE "boot"
#define LOG_LEVEL LOG_LEVEL_DEBUG
#include "aios/aios_log.h"
#include <stdio.h>
#include "plat/blk_hal.h"

/* v0.4.102: boot status, set by boot_fs_init, read by boot_services */
int aios_boot_status = BOOT_FS_OK;

int boot_fs_init(void) {
    /* Platform HAL: probe hardware, init block device */
    if (plat_blk_init() != 0) {
        AIOS_LOG_ERROR("Block device init failed -- entering DEGRADED mode");
        /* Still init VFS + /proc so recovery can read /proc/log etc. */
        vfs_init();
        proc_init();
        vfs_mount("/proc", &procfs_ops, NULL);
        aios_boot_status = BOOT_FS_FATAL;
        return BOOT_FS_FATAL;
    }

    /* Mount root filesystem */
    vfs_init();
    proc_init();
    /* v0.4.112: route ext2 sector I/O through blk_cache. The cache
     * fronts plat_blk_read/write so we get a hit-rate without ext2
     * needing to know anything about caching. */
    blk_cache_init(0);
    blk_cache_register_backend(0, plat_blk_read, plat_blk_write);
    /* v0.4.172: write-back line flushes use multi-block writes (RPi4 CMD25). */
    blk_cache_register_write_multi(0, plat_blk_write_multi);
    /* cache line-fills use multi-block reads (RPi4 CMD18). Drive 1 (log) is
     * left unregistered -- it keeps the single-sector fill by design. */
    blk_cache_register_read_multi(0, plat_blk_read_multi);
    /* discard freed blocks (RPi4 CMD38 erase; no-op on QEMU + the log drive). */
    blk_cache_register_discard(0, plat_blk_discard);
    int fs_err = ext2_init(&ext2, blk_cache_read0, 0);
    if (fs_err == 0) {
        ext2_init_write(&ext2, blk_cache_write0);
        ext2_set_discard(&ext2, blk_cache_discard0);
        vfs_mount("/", &ext2_fs_ops, &ext2);
        vfs_mount("/proc", &procfs_ops, NULL);
        proc_add("fs_thread", 200);
        proc_add("exec_thread", 200);
        proc_add("thread_server", 200);
        LOG_INFO("ext2 + procfs mounted");
        AIOS_LOG_INFO("Filesystems mounted");
    } else {
        AIOS_LOG_ERROR_V("ext2 init failed -- DEGRADED, no disk; err=", fs_err);
        /* /proc still works; user may try to recover */
        vfs_mount("/proc", &procfs_ops, NULL);
        aios_boot_status = BOOT_FS_DEGRADED;
    }

    /* Mount log drive if platform found one.
     *
     * v0.4.100: After plat_blk_init_log, the system disk's virtio state
     * needs a "warmup" read or subsequent reads silently fail.
     * Root cause unconfirmed (suspected stale IRQ state or descriptor race
     * between two virtio-blk devices on QEMU). The warmup read at sector 2
     * (system disk superblock) reliably clears the bad state. */
    if (plat_blk_init_log() == 0) {
        uint8_t warmup_buf[512];
        (void)plat_blk_read(2, warmup_buf);  /* warmup: system disk */

        /* v0.4.112: route log drive through cache too */
        blk_cache_register_backend(1, plat_blk_read_log, plat_blk_write_log);
        int err = ext2_init(&ext2_log, blk_cache_read1, 1);

        if (err == 0) {
            ext2_init_write(&ext2_log, blk_cache_write1);
            vfs_mount("/var/log", &ext2_fs_ops, &ext2_log);
            LOG_INFO("log drive mounted at /var/log");
            AIOS_LOG_INFO("Log drive mounted at /var/log");
        } else {
            AIOS_LOG_WARN_V("Log ext2 failed (not formatted?) err=", err);
        }
    } else {
        AIOS_LOG_INFO("No log drive (optional)");
    }
    return aios_boot_status;
}

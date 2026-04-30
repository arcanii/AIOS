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
#define LOG_MODULE "boot"
#define LOG_LEVEL LOG_LEVEL_DEBUG
#include "aios/aios_log.h"
#include <stdio.h>
#include "plat/blk_hal.h"

void boot_fs_init(void) {
    /* Platform HAL: probe hardware, init block device */
    if (plat_blk_init() != 0) {
        AIOS_LOG_ERROR("Block device init failed");
        return;
    }

    /* Mount root filesystem */
    vfs_init();
    proc_init();
    int fs_err = ext2_init(&ext2, plat_blk_read, 0);
    if (fs_err == 0) {
        ext2_init_write(&ext2, plat_blk_write);
        vfs_mount("/", &ext2_fs_ops, &ext2);
        vfs_mount("/proc", &procfs_ops, NULL);
        proc_add("fs_thread", 200);
        proc_add("exec_thread", 200);
        proc_add("thread_server", 200);
        LOG_INFO("ext2 + procfs mounted");
        AIOS_LOG_INFO("Filesystems mounted");
    } else {
        AIOS_LOG_ERROR_V("ext2 init failed err=", fs_err);
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

        int err = ext2_init(&ext2_log, plat_blk_read_log, 1);

        if (err == 0) {
            ext2_init_write(&ext2_log, plat_blk_write_log);
            vfs_mount("/var/log", &ext2_fs_ops, &ext2_log);
            LOG_INFO("log drive mounted at /var/log");
            AIOS_LOG_INFO("Log drive mounted at /var/log");
        } else {
            AIOS_LOG_WARN_V("Log ext2 failed (not formatted?) err=", err);
        }
    } else {
        AIOS_LOG_INFO("No log drive (optional)");
    }
}

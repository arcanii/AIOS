/* vfs_server.c -- AIOS Virtual Filesystem Server
 *
 * v0.3.x: Standalone PD for path resolution, mount table,
 * and file descriptor management. Forwards actual I/O to
 * fs_server via PPC. This separation protects the VFS
 * namespace from filesystem backend bugs.
 *
 * Current: thin passthrough to fs_server.
 * Planned: mount table, path canonicalization, per-process FD table.
 */
#include <microkit.h>
#include "aios/ipc.h"
#include "aios/channels.h"
#include "aios/util.h"

/* Memory regions (set by Microkit loader) */
uintptr_t vfs_data;   /* shared with orchestrator */
uintptr_t fs_data;     /* shared with fs_server for forwarding */

/* Logging stubs */
void _log_puts(const char *s) { (void)s; }
void _log_put_dec(unsigned long n) { (void)n; }
void _log_flush(void) { }
unsigned long _log_get_time(void) { return 0; }

/* Mount table (future: multiple filesystems) */
#define MAX_MOUNTS 4

typedef struct {
    int active;
    char path[64];     /* mount point */
    int fs_backend_id; /* which fs_server backend to use */
} mount_entry_t;

static mount_entry_t mounts[MAX_MOUNTS];
static int mount_count = 0;

/* Path resolution: canonicalize and find mount point */
static int resolve_mount(const char *path) {
    (void)path;
    /* For now, all paths go to mount 0 (root fs) */
    if (mount_count > 0 && mounts[0].active)
        return 0;
    return -1;
}

/* Forward a VFS command to fs_server unchanged */
static int forward_to_fs(void) {
    /* VFS data and FS data share the same layout (IPC offsets).
     * Copy command block from vfs_data to fs_data, call fs_server,
     * copy results back. */
    volatile uint8_t *vd = (volatile uint8_t *)vfs_data;
    volatile uint8_t *fd = (volatile uint8_t *)fs_data;

    /* Copy command header + filename area (first 0x200 bytes) */
    for (int i = 0; i < 0x200; i++)
        fd[i] = vd[i];

    /* If this is a write command, also copy data area */
    uint32_t cmd = RD32(vfs_data, FS_CMD);
    if (cmd == FS_CMD_WRITE || cmd == FS_CMD_CREATE) {
        uint32_t len = RD32(vfs_data, FS_LENGTH);
        if (len > FS_DATA_MAX) len = FS_DATA_MAX;
        for (uint32_t i = 0; i < len; i++)
            fd[FS_DATA + i] = vd[FS_DATA + i];
    }

    /* PPC to fs_server */
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));

    /* Copy results back to vfs_data */
    for (int i = 0; i < 0x200; i++)
        vd[i] = fd[i];

    /* For read commands, copy data back */
    if (cmd == FS_CMD_READ || cmd == FS_CMD_LIST || cmd == FS_CMD_STAT ||
        cmd == FS_CMD_STAT_EX || cmd == FS_CMD_FSINFO) {
        uint32_t len;
        if (cmd == FS_CMD_LIST) {
            /* LIST: FS_LENGTH = entry count, FS_FILESIZE = byte count */
            len = RD32(fs_data, FS_FILESIZE);
        } else {
            len = RD32(fs_data, FS_LENGTH);
        }
        if (len > FS_DATA_MAX) len = FS_DATA_MAX;
        for (uint32_t i = 0; i < len; i++)
            vd[FS_DATA + i] = fd[FS_DATA + i];
    }

    return (int)RD32(fs_data, FS_STATUS);
}

/* Microkit init */
void init(void) {
    /* Set up default root mount */
    mounts[0].active = 1;
    mounts[0].path[0] = '/';
    mounts[0].path[1] = '\0';
    mounts[0].fs_backend_id = 0;
    mount_count = 1;
}

/* Protected procedure call handler (from orchestrator) */
seL4_MessageInfo_t protected(microkit_channel ch, seL4_MessageInfo_t msginfo) {
    (void)ch; (void)msginfo;

    uint32_t cmd = RD32(vfs_data, FS_CMD);

    /* Path resolution for commands that have a filename */
    if (cmd == FS_CMD_OPEN || cmd == FS_CMD_CREATE ||
        cmd == FS_CMD_DELETE || cmd == FS_CMD_STAT ||
        cmd == FS_CMD_MKDIR || cmd == FS_CMD_RMDIR) {
        int mount_idx = resolve_mount((const char *)(vfs_data + FS_FILENAME));
        if (mount_idx < 0) {
            WR32(vfs_data, FS_STATUS, FS_ST_NOT_FOUND);
            return microkit_msginfo_new(0, 0);
        }
    }

    /* Forward everything to fs_server */
    forward_to_fs();

    return microkit_msginfo_new(0, 0);
}

/* Notification handler */
void notified(microkit_channel ch) {
    (void)ch;
}

/* orchestrator_new.c -- AIOS Orchestrator (Service Router) CORRECT_V2
 *
 * v0.3.x: pure service router. Receives PPC from sandbox kernel,
 * dispatches to fs/auth/net services, returns results.
 * No process management, no scheduling, no slot tracking.
 */
#include <microkit.h>
#include "aios/ipc.h"
#include "aios/channels.h"
#include "aios/auth.h"
#include "aios/version.h"
#include "aios/ring.h"
#include "sys/syscall.h"
#include <kernel/gen_config.h>
#include "arch/aarch64/timer.h"

#define TIMER_IRQ_CH    14      /* channel id for timer IRQ */
#define PREEMPT_MS      10      /* tick interval in ms */

static void arm_preempt_timer(void) {
    uint64_t freq = arch_timer_freq();
    uint64_t now;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    uint64_t target = now + (freq * PREEMPT_MS) / 1000;
    __asm__ volatile("msr cntp_cval_el0, %0" :: "r"(target));
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)1));
}

/* ---- Shared memory (set by Microkit loader via setvar) ---- */
uintptr_t tx_buf;
uintptr_t rx_buf;
uintptr_t sock_data;
uintptr_t fs_data;
uintptr_t vfs_data;    /* shared with vfs_server */
uintptr_t auth_io;
uintptr_t sandbox_io;    /* 4 KB IPC page with sandbox kernel */
uintptr_t sandbox_mem;   /* 128 MB sandbox memory pool */

/* ---- Serial helpers ---- */
static void ser_putc(char c) {
    ring_buf_t *tx = (ring_buf_t *)tx_buf;
    ring_put(tx, c);
}
static void ser_flush(void) { microkit_notify(CH_SERIAL); }
static void ser_puts(const char *s) { while (*s) ser_putc(*s++); }
static void ser_put_dec(unsigned int n) {
    char buf[12]; int i = 0;
    if (n == 0) { ser_putc('0'); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) ser_putc(buf[--i]);
}
static void ser_put_hex64(uint64_t v) {
    ser_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        ser_putc("0123456789abcdef"[(v >> i) & 0xf]);
}

/* ---- Auth sync helpers ---- */
static uint32_t current_uid = 0;
static uint32_t current_gid = 0;

static int __attribute__((unused)) auth_check_file_sync(const char *path, uint32_t mode) {
    (void)path; (void)mode;
    return 0;  /* TODO: implement auth checks */
}

/* ---- FS sync helpers ---- */
#include "orch/orch_fs.inc"

/* ---- CWD for path resolution ---- */
static char cwd[256] = "/";

static void resolve_path(const char *name, char *out, int out_size) {
    if (name[0] == '/') {
        int i = 0;
        while (name[i] && i < out_size - 1) { out[i] = name[i]; i++; }
        out[i] = '\0';
    } else {
        int j = 0, k = 0;
        while (cwd[k] && j < out_size - 2) out[j++] = cwd[k++];
        if (j > 1) out[j++] = '/';
        k = 0;
        while (name[k] && j < out_size - 1) out[j++] = name[k++];
        out[j] = '\0';
    }
}

/* ---- Protected call handler (PPC from sandbox kernel) ---- */
seL4_MessageInfo_t protected(microkit_channel ch, seL4_MessageInfo_t msginfo) {
    (void)msginfo;
    if (ch != CH_SANDBOX) {
        seL4_SetMR(0, -1);
        return microkit_msginfo_new(0, 1);
    }

    int64_t syscall_nr = (int64_t)seL4_GetMR(0);
    int64_t result = -1;

    switch (syscall_nr) {

    /* ---- Serial I/O ---- */
    case SYS_PUTC: {
        char c = (char)seL4_GetMR(1);
        ser_putc(c);
        ser_flush();
        result = 0;
        break;
    }
    case SYS_GETC: {
        ring_buf_t *rx = (ring_buf_t *)rx_buf;
        char c;
        if (ring_get(rx, &c)) {
            result = (int64_t)(unsigned char)c;
        } else {
            result = -2;  /* EAGAIN -- no data, sandbox should yield and retry */
        }
        break;
    }
    case 32: { /* SYS_PUTS_DIRECT */
        volatile char *str = (volatile char *)(sandbox_io + 0x200);
        char buf[256];
        int i = 0;
        while (str[i] && i < 255) { buf[i] = str[i]; i++; }
        buf[i] = '\0';
        ser_puts(buf);
        ser_flush();
        result = 0;
        break;
    }

    /* ---- Filesystem ---- */
    case SYS_OPEN: {
        volatile char *path = (volatile char *)(sandbox_io + 0x200);
        uint32_t flags = (uint32_t)seL4_GetMR(1);
        char raw_name[256];
        int i = 0;
        while (path[i] && i < 255) { raw_name[i] = path[i]; i++; }
        raw_name[i] = '\0';
        char fname[256];
        resolve_path(raw_name, fname, sizeof(fname));
        fs_set_creator_sync(current_uid, current_gid);
        int st = fs_open_sync(fname);
        if (st != 0 && (flags & 0x0040)) {
            st = fs_create_sync(fname);
        }
        if (st == 0) {
            result = RD32(vfs_data, FS_FD);
            seL4_SetMR(1, RD32(vfs_data, FS_FILESIZE));
        } else {
            result = -1;
        }
        break;
    }
    case SYS_READ: {
        uint32_t fd = (uint32_t)seL4_GetMR(1);
        uint32_t offset = (uint32_t)seL4_GetMR(2);
        uint32_t count = (uint32_t)seL4_GetMR(3);
        int st = fs_read_sync(fd, offset, count);
        if (st == 0) {
            uint32_t got = RD32(vfs_data, FS_LENGTH);
            volatile uint8_t *src = (volatile uint8_t *)(vfs_data + FS_DATA);
            volatile uint8_t *dst = (volatile uint8_t *)(sandbox_io + 0x200);
            for (uint32_t j = 0; j < got && j < 3584; j++) dst[j] = src[j];
            result = (int64_t)got;
        } else {
            result = -1;
        }
        break;
    }
    case SYS_WRITE: {
        uint32_t fd = (uint32_t)seL4_GetMR(1);
        uint32_t count = (uint32_t)seL4_GetMR(2);
        if (count > 3584) count = 3584;
        volatile uint8_t *src = (volatile uint8_t *)(sandbox_io + 0x200);
        volatile uint8_t *dst = (volatile uint8_t *)(vfs_data + FS_DATA);
        for (uint32_t j = 0; j < count; j++) dst[j] = src[j];
        int st = fs_write_sync(fd, count);
        if (st == 0) {
            result = (int64_t)RD32(vfs_data, FS_LENGTH);
        } else {
            result = -1;
        }
        break;
    }
    case SYS_CLOSE: {
        uint32_t fd = (uint32_t)seL4_GetMR(1);
        result = fs_close_sync(fd);
        break;
    }
    case SYS_UNLINK: {
        volatile char *path = (volatile char *)(sandbox_io + 0x200);
        char raw[256], fname[256];
        int i = 0;
        while (path[i] && i < 255) { raw[i] = path[i]; i++; }
        raw[i] = '\0';
        resolve_path(raw, fname, sizeof(fname));
        result = fs_delete_sync(fname);
        break;
    }
    case SYS_READDIR: {
        volatile char *path = (volatile char *)(sandbox_io + 0x200);
        char dir[256];
        int i = 0;
        while (path[i] && i < 255) { dir[i] = path[i]; i++; }
        dir[i] = '\0';
        char fname[256];
        resolve_path(dir, fname, sizeof(fname));
        int st = fs_list_sync(fname);
        if (st == 0) {
            uint32_t count = RD32(vfs_data, FS_LENGTH);
            uint32_t total = RD32(vfs_data, FS_FILESIZE);
            volatile uint8_t *src = (volatile uint8_t *)(vfs_data + FS_DATA);
            volatile uint8_t *dst = (volatile uint8_t *)(sandbox_io + 0x200);
            if (total > 3584) total = 3584;
            for (uint32_t j = 0; j < total; j++) dst[j] = src[j];
            seL4_SetMR(1, (seL4_Word)total);
            result = (int64_t)count;
        } else {
            result = -1;
        }
        break;
    }
    case SYS_STAT: {
        volatile char *path = (volatile char *)(sandbox_io + 0x200);
        char raw[256], fname[256];
        int i = 0;
        while (path[i] && i < 255) { raw[i] = path[i]; i++; }
        raw[i] = '\0';
        resolve_path(raw, fname, sizeof(fname));
        uint32_t st_size = 0, st_uid = 0, st_gid = 0, st_mode = 0, st_mtime = 0;
        result = fs_stat_ex_sync(fname, &st_size, &st_uid, &st_gid, &st_mode, &st_mtime);
        if (result == 0) {
            seL4_SetMR(1, (seL4_Word)st_size);
            seL4_SetMR(2, (seL4_Word)st_uid);
            seL4_SetMR(3, (seL4_Word)st_gid);
            seL4_SetMR(4, (seL4_Word)st_mode);
            seL4_SetMR(5, (seL4_Word)st_mtime);
        }
        break;
    }
    case SYS_MKDIR: {
        volatile char *path = (volatile char *)(sandbox_io + 0x200);
        char raw[256], fname[256];
        int i = 0;
        while (path[i] && i < 255) { raw[i] = path[i]; i++; }
        raw[i] = '\0';
        resolve_path(raw, fname, sizeof(fname));
        result = fs_mkdir_sync(fname);
        break;
    }
    case SYS_RMDIR: {
        volatile char *path = (volatile char *)(sandbox_io + 0x200);
        char raw[256], fname[256];
        int i = 0;
        while (path[i] && i < 255) { raw[i] = path[i]; i++; }
        raw[i] = '\0';
        resolve_path(raw, fname, sizeof(fname));
        result = fs_rmdir_sync(fname);
        break;
    }
    case SYS_RENAME: {
        volatile char *buf = (volatile char *)(sandbox_io + 0x200);
        char old_raw[128], new_raw[128];
        char old_path[256], new_path[256];
        int i = 0, j = 0;
        while (buf[i] && i < 127) { old_raw[j++] = buf[i++]; }
        old_raw[j] = '\0';
        i++;  /* skip null separator */
        j = 0;
        while (buf[i] && i < 255 && j < 127) { new_raw[j++] = buf[i++]; }
        new_raw[j] = '\0';
        resolve_path(old_raw, old_path, sizeof(old_path));
        resolve_path(new_raw, new_path, sizeof(new_path));
        result = fs_rename_sync(old_path, new_path);
        break;
    }
    case SYS_LSEEK: {
        result = 0;  /* handled client-side */
        break;
    }
    case SYS_ACCESS: {
        result = 0;  /* TODO: real access check */
        break;
    }
    case SYS_GETCWD: {
        volatile char *dst = (volatile char *)(sandbox_io + 0x200);
        int i = 0;
        while (cwd[i] && i < 255) { dst[i] = cwd[i]; i++; }
        dst[i] = '\0';
        result = 0;
        break;
    }
    case SYS_CHDIR: {
        volatile char *path = (volatile char *)(sandbox_io + 0x200);
        char raw[256], fname[256];
        int i = 0;
        while (path[i] && i < 255) { raw[i] = path[i]; i++; }
        raw[i] = '\0';
        resolve_path(raw, fname, sizeof(fname));
        /* Verify directory exists */
        int st = fs_stat_sync(fname);
        if (st == 0) {
            int k = 0;
            while (fname[k] && k < 255) { cwd[k] = fname[k]; k++; }
            cwd[k] = '\0';
            result = 0;
        } else {
            result = -1;
        }
        break;
    }
    case SYS_CHMOD: {
        volatile char *path = (volatile char *)(sandbox_io + 0x200);
        uint32_t mode = (uint32_t)seL4_GetMR(1);
        char raw[256], fname[256];
        int i = 0;
        while (path[i] && i < 255) { raw[i] = path[i]; i++; }
        raw[i] = '\0';
        resolve_path(raw, fname, sizeof(fname));
        result = fs_chmod_sync(fname, mode);
        break;
    }
    case SYS_CHOWN: {
        volatile char *path = (volatile char *)(sandbox_io + 0x200);
        uint32_t uid = (uint32_t)seL4_GetMR(1);
        uint32_t gid = (uint32_t)seL4_GetMR(2);
        char raw[256], fname[256];
        int i = 0;
        while (path[i] && i < 255) { raw[i] = path[i]; i++; }
        raw[i] = '\0';
        resolve_path(raw, fname, sizeof(fname));
        result = fs_chown_sync(fname, uid, gid);
        break;
    }

    /* ---- Exec: load binary from fs into sandbox_mem ---- */
    case SYS_EXEC: {
        volatile char *fn = (volatile char *)(sandbox_io + 0x200);
        char raw[256], path[256];
        int i = 0;
        while (fn[i] && i < 255) { raw[i] = fn[i]; i++; }
        raw[i] = '\0';
        resolve_path(raw, path, sizeof(path));
        /* Offset into sandbox_mem where code should be loaded */
        uint64_t mem_offset = seL4_GetMR(1);
        volatile uint8_t *code_dst = (volatile uint8_t *)(sandbox_mem + mem_offset);
        int st = fs_open_sync(path);
        if (st != 0) { result = -1; break; }
        uint32_t fd = RD32(vfs_data, FS_FD);
        uint32_t file_size = RD32(vfs_data, FS_FILESIZE);
        uint32_t loaded = 0;
        while (loaded < file_size) {
            uint32_t chunk = file_size - loaded;
            if (chunk > 4096) chunk = 4096;
            int r = fs_read_sync(fd, loaded, chunk);
            if (r != 0) break;
            uint32_t got = RD32(vfs_data, FS_LENGTH);
            volatile uint8_t *src = (volatile uint8_t *)(vfs_data + FS_DATA);
            for (uint32_t j = 0; j < got; j++) code_dst[loaded + j] = src[j];
            loaded += got;
            if (got == 0) break;
        }
        fs_close_sync(fd);
        seL4_SetMR(1, (seL4_Word)loaded);
        result = (loaded > 0) ? 0 : -1;
        break;
    }

    /* ---- Time ---- */
    case SYS_LOGIN: {
        /* Read username and password from sandbox_io */
        volatile char *uname = (volatile char *)(sandbox_io + 0x200);
        volatile char *passwd = (volatile char *)(sandbox_io + 0x300);
        /* Copy to auth_io */
        volatile char *au = (volatile char *)(auth_io + AUTH_USERNAME);
        volatile char *ap = (volatile char *)(auth_io + AUTH_PASSWORD);
        int i = 0;
        while (uname[i] && i < 31) { au[i] = uname[i]; i++; }
        au[i] = '\0';
        i = 0;
        while (passwd[i] && i < 63) { ap[i] = passwd[i]; i++; }
        ap[i] = '\0';
        /* Clear password from sandbox_io */
        for (int j = 0; j < 64; j++) passwd[j] = 0;
        /* Call auth server */
        WR32(auth_io, AUTH_CMD, AUTH_CMD_LOGIN);
        microkit_ppcall(CH_AUTH, microkit_msginfo_new(0, 0));
        int status = (int)(int32_t)RD32(auth_io, AUTH_STATUS);
        if (status == 0) {
            uint32_t uid = RD32(auth_io, AUTH_UID);
            uint32_t gid = RD32(auth_io, AUTH_GID);
            seL4_SetMR(1, (seL4_Word)uid);
            seL4_SetMR(2, (seL4_Word)gid);
            result = 0;
        } else {
            result = -1;
        }
        break;
    }
    case SYS_TIME: {
        uint64_t ts = arch_timestamp();
        uint64_t freq = arch_timer_freq();
        uint64_t secs = ts / freq;
        seL4_SetMR(1, (seL4_Word)secs);
        seL4_SetMR(2, (seL4_Word)freq);
        result = 0;
        break;
    }

    /* ---- Identity (stub -- sandbox kernel tracks real uid per process) ---- */
    case SYS_GETUID:  result = current_uid; break;
    case SYS_GETGID:  result = current_gid; break;
    case SYS_GETEUID: result = current_uid; break;
    case SYS_GETEGID: result = current_gid; break;

    /* ---- System control ---- */
    case SYS_SYNC: {
        /* Flush filesystem caches to disk */
        WR32(vfs_data, FS_CMD, FS_CMD_SYNC);
        microkit_ppcall(CH_VFS, microkit_msginfo_new(0, 0));
        result = (int64_t)RD32(vfs_data, FS_STATUS);
        break;
    }
    case SYS_SHUTDOWN: {
        uint32_t flags = (uint32_t)seL4_GetMR(1);
        (void)flags;
        ser_puts("\n[orchestrator] SHUTDOWN: syncing filesystems...\n");
        ser_flush();

        /* Sync filesystem */
        WR32(vfs_data, FS_CMD, FS_CMD_SYNC);
        microkit_ppcall(CH_VFS, microkit_msginfo_new(0, 0));

        ser_puts("[orchestrator] SHUTDOWN: filesystems synced\n");
        ser_puts("[orchestrator] SHUTDOWN: halting system\n");
        ser_puts("\nSystem halted. It is safe to power off.\n\n");
        ser_flush();

        /* Halt: loop with WFI (wait for interrupt)
         * On QEMU, the user can exit with Ctrl-A X.
         * PSCI via HVC/SMC is trapped by seL4, so we just halt cleanly. */
        for (;;) { __asm__ volatile("wfi"); }
        break;
    }

    default:
        ser_puts("ORCH: unknown syscall ");
        ser_put_dec((unsigned int)syscall_nr);
        ser_puts("\n");
        ser_flush();
        result = -1;
        break;
    }

    seL4_SetMR(0, (seL4_Word)result);
    return microkit_msginfo_new(0, 6);
}

/* ---- Init ---- */
void init(void) {
    ser_puts("\n============ Open Aries ================\n");
    ser_puts("  " AIOS_VERSION_FULL "\n");
    ser_puts("  Kernel:  seL4 14.0.0 (Microkit 2.1.0)\n");
    ser_puts("  CPUs:    ");
    ser_put_dec(CONFIG_MAX_NUM_NODES);
    ser_puts(" cores (SMP)\n");
    ser_puts("  Sandbox: 1 PD (128 MB user-space kernel)\n");
    const char *fsname = fs_fsinfo_sync();
    ser_puts("  FS:      ");
    ser_puts(fsname);
    ser_puts("\n");
    ser_puts("  Drivers: PL011 UART, virtio-blk, virtio-net\n");
    ser_puts("============ Open Aries ================\n\n");
    ser_flush();

    /* Start preemption timer */
    arm_preempt_timer();
    ser_puts("[orchestrator] preempt timer started (" );
    ser_puts("10ms)\n");
}

/* ---- Notification handler ---- */
void notified(microkit_channel ch) {
    if (ch == TIMER_IRQ_CH) {
        /* Timer tick — notify sandbox for preemption */
        microkit_irq_ack(ch);
        microkit_notify(CH_SANDBOX);
        arm_preempt_timer();
    }
}

/* ---- Fault handler (sandbox crash) ---- */
seL4_Bool fault(microkit_child child, microkit_msginfo msginfo, microkit_msginfo *reply_msginfo) {
    (void)reply_msginfo;
    uint64_t fault_ip   = seL4_GetMR(0);
    uint64_t fault_addr = seL4_GetMR(1);
    ser_puts("\nFAULT: sandbox crash at pc=");
    ser_put_hex64(fault_ip);
    ser_puts(" addr=");
    ser_put_hex64(fault_addr);
    ser_puts("\n");
    ser_flush();
    (void)msginfo;
    microkit_pd_restart(child, 0x200000);
    return seL4_True;
}

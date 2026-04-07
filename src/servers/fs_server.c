/* AIOS servers/fs_server.c -- Filesystem IPC server
 *
 * Handles FS_LS, FS_CAT, FS_STAT, FS_MKDIR, FS_WRITE_FILE,
 * FS_UNLINK, FS_UNAME, FS_RENAME via VFS dispatch.
 * Includes badge-based write permission checks.
 */
#include <stdio.h>
#include <sel4/sel4.h>
#include "aios/root_shared.h"
#include "aios/version.h"
#include "aios/vfs.h"
#include "aios/ext2.h"
#include "aios/procfs.h"

int fs_check_write_perm(int badge) {
    if (badge == 0) return 1;  /* unbadged = internal (root) */
    int idx = badge - 1;
    if (idx < 0 || idx >= MAX_ACTIVE_PROCS) return 0;
    if (!active_procs[idx].active) return 0;
    return (active_procs[idx].uid == 0);  /* only root can write */
}

int fs_check_path_write(int badge, const char *path) {
    if (fs_check_write_perm(badge)) return 1;
    /* Non-root: deny writes to /etc/ */
    if (path[0] == '/' && path[1] == 'e' && path[2] == 't' &&
        path[3] == 'c' && (path[4] == '/' || path[4] == '\0'))
        return 0;
    /* Non-root: deny writes to /bin/ */
    if (path[0] == '/' && path[1] == 'b' && path[2] == 'i' &&
        path[3] == 'n' && (path[4] == '/' || path[4] == '\0'))
        return 0;
    /* Allow other writes */
    return 1;
}

/* Filesystem IPC thread — runs in root task VSpace */
void fs_thread_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg0;
    static char fs_buf[4096];
    (void)ep; /* used below in Recv */

    /* quiet */

    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);
        int fs_badge = (int)badge;


        switch (label) {
        case FS_LS: {
            /* Multi-round protocol: MR0=path_len, MR1=offset, MR2..=path */
            seL4_Word path_len = seL4_GetMR(0);
            seL4_Word ls_offset = seL4_GetMR(1);
            char ls_path[128];
            int lpl = (path_len > 127) ? 127 : (int)path_len;
            int ls_mr = 2;  /* path starts at MR2 (MR1 is offset) */
            for (int i = 0; i < lpl; i++) {
                if (i % 8 == 0 && i > 0) ls_mr++;
                ls_path[i] = (char)((seL4_GetMR(ls_mr) >> ((i % 8) * 8)) & 0xFF);
            }
            ls_path[lpl] = '\0';
            if (lpl == 0) { ls_path[0] = '/'; ls_path[1] = '\0'; }

            /* Only fetch listing on first round (offset==0) */
            static int fs_ls_total = 0;
            if (ls_offset == 0) {
                fs_ls_total = vfs_list(ls_path, fs_buf, sizeof(fs_buf));
                if (fs_ls_total < 0) fs_ls_total = 0;
            }

            /* Send chunk starting at offset */
            int remaining = fs_ls_total - (int)ls_offset;
            if (remaining < 0) remaining = 0;
            int mrs = (remaining + 7) / 8;
            if (mrs > (int)seL4_MsgMaxLength - 1) mrs = seL4_MsgMaxLength - 1;
            int chunk = mrs * 8;
            if (chunk > remaining) chunk = remaining;
            seL4_SetMR(0, (seL4_Word)fs_ls_total);
            for (int i = 0; i < mrs; i++) {
                seL4_Word w = 0;
                for (int j = 0; j < 8 && i*8+j < chunk; j++)
                    w |= ((seL4_Word)(uint8_t)fs_buf[(int)ls_offset + i*8+j]) << (j*8);
                seL4_SetMR(i + 1, w);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, mrs + 1));
            break;
        }
        case FS_CAT: {
            seL4_Word path_len = seL4_GetMR(0);
            char path[128];
            int pl = (path_len > 127) ? 127 : (int)path_len;
            int mr_idx = 1;
            for (int i = 0; i < pl; i++) {
                if (i % 8 == 0 && i > 0) mr_idx++;
                path[i] = (char)((seL4_GetMR(mr_idx) >> ((i % 8) * 8)) & 0xFF);
            }
            path[pl] = '\0';

            int len = vfs_read(path, fs_buf, sizeof(fs_buf));
            if (len < 0) len = 0;
            int mrs = (len + 7) / 8;
            if (mrs > (int)seL4_MsgMaxLength - 1) mrs = seL4_MsgMaxLength - 1;
            seL4_SetMR(0, (seL4_Word)len);
            for (int i = 0; i < mrs; i++) {
                seL4_Word w = 0;
                for (int j = 0; j < 8 && i*8+j < len; j++)
                    w |= ((seL4_Word)(uint8_t)fs_buf[i*8+j]) << (j*8);
                seL4_SetMR(i + 1, w);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, mrs + 1));
            break;
        }
        case FS_STAT: {
            seL4_Word path_len = seL4_GetMR(0);
            char st_path[128];
            int spl = (path_len > 127) ? 127 : (int)path_len;
            int st_mr = 1;
            for (int i = 0; i < spl; i++) {
                if (i % 8 == 0 && i > 0) st_mr++;
                st_path[i] = (char)((seL4_GetMR(st_mr) >> ((i % 8) * 8)) & 0xFF);
            }
            st_path[spl] = '\0';

            uint32_t mode, size;
            if (vfs_stat(st_path, &mode, &size) == 0) {
                seL4_SetMR(0, 1);
                seL4_SetMR(1, (seL4_Word)mode);
                seL4_SetMR(2, (seL4_Word)size);
                seL4_SetMR(3, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 4));
            } else {
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            }
            break;
        }
        case FS_MKDIR: {
            seL4_Word path_len = seL4_GetMR(0);
            char mk_path[128];
            int mkpl = (path_len > 127) ? 127 : (int)path_len;
            int mk_mr = 1;
            for (int i = 0; i < mkpl; i++) {
                if (i % 8 == 0 && i > 0) mk_mr++;
                mk_path[i] = (char)((seL4_GetMR(mk_mr) >> ((i % 8) * 8)) & 0xFF);
            }
            mk_path[mkpl] = '\0';
            if (!fs_check_path_write(fs_badge, mk_path)) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int ret = vfs_mkdir(mk_path);
            seL4_SetMR(0, (seL4_Word)(ret >= 0 ? 0 : -1));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case FS_WRITE_FILE: {
            /* MR0=path_len, MR1..=path, then data_len + data */
            seL4_Word path_len = seL4_GetMR(0);
            char wr_path_pre[128];
            /* Peek at path for permission check */
            int wrpl_pre = (path_len > 127) ? 127 : (int)path_len;
            int wr_mr_pre = 1;
            for (int i = 0; i < wrpl_pre; i++) {
                if (i % 8 == 0 && i > 0) wr_mr_pre++;
                wr_path_pre[i] = (char)((seL4_GetMR(wr_mr_pre) >> ((i % 8) * 8)) & 0xFF);
            }
            wr_path_pre[wrpl_pre] = '\0';
            if (!fs_check_path_write(fs_badge, wr_path_pre)) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            char wr_path[128];
            int wrpl = (path_len > 127) ? 127 : (int)path_len;
            int wr_mr = 1;
            for (int i = 0; i < wrpl; i++) {
                if (i % 8 == 0 && i > 0) wr_mr++;
                wr_path[i] = (char)((seL4_GetMR(wr_mr) >> ((i % 8) * 8)) & 0xFF);
            }
            wr_path[wrpl] = '\0';
            wr_mr++;
            seL4_Word data_len = seL4_GetMR(wr_mr++);
            char wr_data[512];
            int dl = (data_len > 511) ? 511 : (int)data_len;
            for (int i = 0; i < dl; i++) {
                if (i % 8 == 0 && i > 0) wr_mr++;
                wr_data[i] = (char)((seL4_GetMR(wr_mr) >> ((i % 8) * 8)) & 0xFF);
            }
            wr_data[dl] = '\0';
            int ret = vfs_create(wr_path, wr_data, dl);
            seL4_SetMR(0, (seL4_Word)(ret >= 0 ? 0 : -1));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case FS_UNLINK: {
            seL4_Word path_len = seL4_GetMR(0);
            char rm_path[128];
            int rmpl = (path_len > 127) ? 127 : (int)path_len;
            int rm_mr = 1;
            for (int i = 0; i < rmpl; i++) {
                if (i % 8 == 0 && i > 0) rm_mr++;
                rm_path[i] = (char)((seL4_GetMR(rm_mr) >> ((i % 8) * 8)) & 0xFF);
            }
            rm_path[rmpl] = '\0';
            if (!fs_check_path_write(fs_badge, rm_path)) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int ret = vfs_unlink(rm_path);
            seL4_SetMR(0, (seL4_Word)(ret >= 0 ? 0 : -1));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case FS_UNAME: {
            /* Return system info packed in MRs:
             * MR0-1: sysname (16 bytes)
             * MR2-3: nodename (16 bytes)  
             * MR4-5: release (16 bytes)
             * MR6-7: version (16 bytes)
             * MR8-9: machine (16 bytes) */
            char info[80];
            for (int i = 0; i < 80; i++) info[i] = 0;
            
            /* sysname */
            const char *s = "AIOS";
            for (int i = 0; s[i] && i < 15; i++) info[i] = s[i];
            
            /* nodename — read from ext2 /etc/hostname */
            char hname[16];
            for (int i = 0; i < 16; i++) hname[i] = 0;
            int hlen = vfs_read("/etc/hostname", hname, 15);
            if (hlen > 0) {
                /* Strip trailing newline */
                if (hname[hlen-1] == '\n') hname[hlen-1] = 0;
            } else {
                hname[0] = 'a'; hname[1] = 'i'; hname[2] = 'o'; hname[3] = 's';
            }
            for (int i = 0; i < 16; i++) info[16 + i] = hname[i];
            
            /* release */
            s = AIOS_VERSION_STR;
            for (int i = 0; s[i] && i < 15; i++) info[32 + i] = s[i];
            
            /* version — seL4 + build info */
            const char *ver = "seL4 15.0.0 SMP #" _AIOS_XSTR(AIOS_BUILD_NUMBER);
            for (int i = 0; ver[i] && i < 15; i++) info[48 + i] = ver[i];
            
            /* machine */
            s = "aarch64";
            for (int i = 0; s[i] && i < 15; i++) info[64 + i] = s[i];
            
            /* Pack into MRs (8 bytes per MR) */
            for (int i = 0; i < 10; i++) {
                seL4_Word w = 0;
                for (int j = 0; j < 8; j++)
                    w |= ((seL4_Word)(uint8_t)info[i*8 + j]) << (j * 8);
                seL4_SetMR(i, w);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 10));
            break;
        }
        case FS_RENAME: {
            seL4_Word old_len = seL4_GetMR(0);
            char old_path[128], new_path[128];
            int opl = (old_len > 127) ? 127 : (int)old_len;
            int rmr = 1;
            for (int i = 0; i < opl; i++) {
                if (i % 8 == 0 && i > 0) rmr++;
                old_path[i] = (char)((seL4_GetMR(rmr) >> ((i % 8) * 8)) & 0xFF);
            }
            old_path[opl] = '\0';
            rmr++;
            seL4_Word new_len = seL4_GetMR(rmr++);
            int npl = (new_len > 127) ? 127 : (int)new_len;
            for (int i = 0; i < npl; i++) {
                if (i % 8 == 0 && i > 0) rmr++;
                new_path[i] = (char)((seL4_GetMR(rmr) >> ((i % 8) * 8)) & 0xFF);
            }
            new_path[npl] = '\0';
            if (!fs_check_path_write(fs_badge, new_path) ||
                !fs_check_path_write(fs_badge, old_path)) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int ret = vfs_rename(old_path, new_path);
            seL4_SetMR(0, (seL4_Word)(ret >= 0 ? 0 : -1));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case FS_APPEND: {
            /* v0.4.66: server-side append -- read existing + append new data.
             * MR layout same as FS_WRITE_FILE: path_len, path, data_len, data */
            seL4_Word path_len = seL4_GetMR(0);
            char ap_path[128];
            int appl = (path_len > 127) ? 127 : (int)path_len;
            int ap_mr = 1;
            for (int i = 0; i < appl; i++) {
                if (i % 8 == 0 && i > 0) ap_mr++;
                ap_path[i] = (char)((seL4_GetMR(ap_mr) >> ((i % 8) * 8)) & 0xFF);
            }
            ap_path[appl] = '\0';
            if (!fs_check_path_write(fs_badge, ap_path)) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            ap_mr++;
            seL4_Word data_len = seL4_GetMR(ap_mr++);
            char ap_new[512];
            int anl = (data_len > 511) ? 511 : (int)data_len;
            for (int i = 0; i < anl; i++) {
                if (i % 8 == 0 && i > 0) ap_mr++;
                ap_new[i] = (char)((seL4_GetMR(ap_mr) >> ((i % 8) * 8)) & 0xFF);
            }
            /* Read existing file content */
            static char ap_buf[4096];
            int existing = vfs_read(ap_path, ap_buf, (int)sizeof(ap_buf) - anl - 1);
            if (existing < 0) existing = 0;
            /* Append new data */
            for (int i = 0; i < anl && existing + i < 4095; i++)
                ap_buf[existing + i] = ap_new[i];
            int total = existing + anl;
            if (total > 4095) total = 4095;
            int ret = vfs_create(ap_path, ap_buf, total);
            seL4_SetMR(0, (seL4_Word)(ret >= 0 ? 0 : -1));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        default:
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }

        /* Check for exited forked children after processing each message */
    }
}


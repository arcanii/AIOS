/* ssh_sftp.c -- minimal SFTP (v3) subsystem server over the SSH channel.
 *
 * v0.4.178: enables `sftp` and modern `scp` (which speaks SFTP) to/from AIOS.
 * The server runs INSIDE sshd on the session channel after the client requests
 * the "sftp" subsystem -- it does file I/O directly through libaios_posix
 * (open/pread/pwrite/getdents/...) and frames SFTP packets as CHANNEL_DATA.
 * It never touches the shell-relay pipe path, so it sidesteps the A72 pipe
 * output-drain race entirely.
 *
 * Protocol: draft-ietf-secsh-filexfer-02 (SFTP v3), the version OpenSSH uses.
 * Supported: INIT/VERSION, REALPATH, STAT/LSTAT/FSTAT, OPEN/READ/WRITE/CLOSE,
 * OPENDIR/READDIR, MKDIR/RMDIR, REMOVE, RENAME, SETSTAT/FSETSTAT(size only).
 */

#include "ssh_session.h"
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

/* ---- SFTP packet types ---- */
#define SSH_FXP_INIT            1
#define SSH_FXP_VERSION         2
#define SSH_FXP_OPEN            3
#define SSH_FXP_CLOSE           4
#define SSH_FXP_READ            5
#define SSH_FXP_WRITE           6
#define SSH_FXP_LSTAT           7
#define SSH_FXP_FSTAT           8
#define SSH_FXP_SETSTAT         9
#define SSH_FXP_FSETSTAT        10
#define SSH_FXP_OPENDIR         11
#define SSH_FXP_READDIR         12
#define SSH_FXP_REMOVE          13
#define SSH_FXP_MKDIR           14
#define SSH_FXP_RMDIR           15
#define SSH_FXP_REALPATH        16
#define SSH_FXP_STAT            17
#define SSH_FXP_RENAME          18
#define SSH_FXP_STATUS          101
#define SSH_FXP_HANDLE          102
#define SSH_FXP_DATA            103
#define SSH_FXP_NAME            104
#define SSH_FXP_ATTRS           105

/* ---- SFTP status codes ---- */
#define SSH_FX_OK               0
#define SSH_FX_EOF              1
#define SSH_FX_NO_SUCH_FILE     2
#define SSH_FX_PERMISSION_DENIED 3
#define SSH_FX_FAILURE          4
#define SSH_FX_OP_UNSUPPORTED   8

/* ---- attribute flags ---- */
#define SSH_FILEXFER_ATTR_SIZE        0x00000001
#define SSH_FILEXFER_ATTR_UIDGID      0x00000002
#define SSH_FILEXFER_ATTR_PERMISSIONS 0x00000004
#define SSH_FILEXFER_ATTR_ACMODTIME   0x00000008

/* ---- open pflags ---- */
#define SSH_FXF_READ   0x01
#define SSH_FXF_WRITE  0x02
#define SSH_FXF_APPEND 0x04
#define SSH_FXF_CREAT  0x08
#define SSH_FXF_TRUNC  0x10
#define SSH_FXF_EXCL   0x20

#define SFTP_VERSION   3

/* Our channel receive window (matches SSH_CHAN_WINDOW in ssh_channel.c, which
 * initialised s->server_window). Replenished as the client sends WRITE data. */
#define SFTP_CHAN_WINDOW (64 * 1024)

/* Buffers. The SSH channel max packet is reduced (ssh_channel.c) to fit
 * SSH_BUF_SIZE, so the client chunks CHANNEL_DATA; we reassemble full SFTP
 * packets here. SFTP_MAX must hold the largest single packet (a ~32 KB WRITE
 * plus header). These are file-scope (sshd has morecore-backed BSS). */
#define SFTP_MAX     40000
#define SFTP_DATAMAX 32768

static uint8_t sin_buf[SFTP_MAX];   /* inbound SFTP reassembly */
static int     sin_len;
static uint8_t out_buf[SFTP_MAX];   /* one outbound SFTP packet */
static uint8_t io_buf[SFTP_DATAMAX];/* file read/write staging */

/* ---- open handles ---- */
#define SFTP_MAX_HANDLES 8
typedef struct {
    int   used;
    int   is_dir;
    int   fd;        /* file: open fd */
    DIR  *dir;       /* dir: opendir handle */
    int   dir_eof;   /* dir: all entries returned */
    char  path[256];
} sftp_handle_t;
static sftp_handle_t handles[SFTP_MAX_HANDLES];

/* ---- little wire helpers (SSH big-endian) ---- */
static uint64_t get_u64(const uint8_t *b) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | b[i]; return v;
}
static void put_u64(uint8_t *b, uint64_t v) {
    for (int i = 7; i >= 0; i--) { b[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}

/* ----------------------------------------------------------------
 * Send `len` bytes of SFTP data as CHANNEL_DATA, chunked to fit the SSH
 * packet buffer and respecting the client window. Returns 0 / -1.
 * ---------------------------------------------------------------- */
static int sftp_send_raw(ssh_session_t *s, const uint8_t *data, int len)
{
    int off = 0;
    uint8_t pkt[SSH_BUF_SIZE];
    while (off < len) {
        int chunk = len - off;
        /* data + 9-byte CHANNEL_DATA header must stay <= SSH_MAX_PAYLOAD (2048) */
        if (chunk > 2000) chunk = 2000;

        /* Wait for client window if needed (read WINDOW_ADJUST). */
        int spins = 0;
        while (s->client_window < (uint32_t)chunk) {
            uint8_t rp[SSH_BUF_SIZE]; int rl = 0;
            if (ssh_read_packet(s, rp, &rl) < 0 || rl < 1) return -1;
            if (rp[0] == SSH_MSG_CHANNEL_WINDOW_ADJUST && rl >= 9)
                s->client_window += ssh_get_u32(rp + 5);
            else if (rp[0] == SSH_MSG_CHANNEL_CLOSE ||
                     rp[0] == SSH_MSG_DISCONNECT) return -1;
            if (++spins > 10000) return -1;
        }

        int o = 0;
        pkt[o++] = SSH_MSG_CHANNEL_DATA;
        ssh_put_u32(pkt + o, s->client_channel); o += 4;
        ssh_put_u32(pkt + o, (uint32_t)chunk);    o += 4;
        memcpy(pkt + o, data + off, chunk);       o += chunk;
        if (ssh_write_packet(s, pkt, o) < 0) return -1;
        s->client_window -= (uint32_t)chunk;
        off += chunk;
    }
    return 0;
}

/* Send a complete SFTP packet: 4-byte length prefix + body. */
static int sftp_send(ssh_session_t *s, const uint8_t *body, int blen)
{
    uint8_t hdr[4];
    ssh_put_u32(hdr, (uint32_t)blen);
    if (sftp_send_raw(s, hdr, 4) < 0) return -1;
    return sftp_send_raw(s, body, blen);
}

/* SSH_FXP_STATUS reply */
static int reply_status(ssh_session_t *s, uint32_t id, uint32_t code)
{
    uint8_t *p = out_buf; int o = 0;
    p[o++] = SSH_FXP_STATUS;
    ssh_put_u32(p + o, id);   o += 4;
    ssh_put_u32(p + o, code); o += 4;
    ssh_put_u32(p + o, 0);    o += 4;   /* error message (empty string) */
    ssh_put_u32(p + o, 0);    o += 4;   /* language tag (empty string) */
    return sftp_send(s, p, o);
}

/* errno -> SFTP status code */
static uint32_t errno_to_fx(int e)
{
    if (e == ENOENT) return SSH_FX_NO_SUCH_FILE;
    if (e == EACCES || e == EPERM) return SSH_FX_PERMISSION_DENIED;
    return SSH_FX_FAILURE;
}

/* Append a v3 ATTRS block from a stat. Returns new offset. */
static int put_attrs(uint8_t *p, int o, struct stat *st)
{
    ssh_put_u32(p + o, SSH_FILEXFER_ATTR_SIZE |
                       SSH_FILEXFER_ATTR_PERMISSIONS); o += 4;
    put_u64(p + o, (uint64_t)st->st_size);  o += 8;
    ssh_put_u32(p + o, (uint32_t)st->st_mode); o += 4;
    return o;
}

/* Append an SSH string (u32 len + bytes). */
static int put_str(uint8_t *p, int o, const char *str, int len)
{
    ssh_put_u32(p + o, (uint32_t)len); o += 4;
    if (len > 0) { memcpy(p + o, str, len); o += len; }
    return o;
}

/* Build an `ls -l`-style longname for READDIR display. */
static int build_longname(char *buf, const char *name, struct stat *st)
{
    char perms[11];
    uint32_t m = st->st_mode;
    perms[0] = S_ISDIR(m) ? 'd' : '-';
    perms[1] = (m & 0400) ? 'r' : '-';
    perms[2] = (m & 0200) ? 'w' : '-';
    perms[3] = (m & 0100) ? 'x' : '-';
    perms[4] = (m & 0040) ? 'r' : '-';
    perms[5] = (m & 0020) ? 'w' : '-';
    perms[6] = (m & 0010) ? 'x' : '-';
    perms[7] = (m & 0004) ? 'r' : '-';
    perms[8] = (m & 0002) ? 'w' : '-';
    perms[9] = (m & 0001) ? 'x' : '-';
    perms[10] = 0;
    /* "perms 1 0 0 <size> <name>" -- snprintf keeps it simple */
    return snprintf(buf, 320, "%s 1 0 0 %lu %s",
                    perms, (unsigned long)st->st_size, name);
}

/* Read an SSH string field from an SFTP packet body at *po into dst (NUL
 * terminated, capped). Returns 0 on success, -1 on malformed. */
static int get_pstr(const uint8_t *b, int blen, int *po, char *dst, int dcap)
{
    if (*po + 4 > blen) return -1;
    uint32_t n = ssh_get_u32(b + *po); *po += 4;
    if (*po + (int)n > blen) return -1;
    int c = (int)n < dcap - 1 ? (int)n : dcap - 1;
    memcpy(dst, b + *po, c); dst[c] = 0;
    *po += (int)n;
    return 0;
}

static sftp_handle_t *handle_alloc(void)
{
    for (int i = 0; i < SFTP_MAX_HANDLES; i++)
        if (!handles[i].used) { memset(&handles[i], 0, sizeof(handles[i]));
                                handles[i].used = 1; return &handles[i]; }
    return NULL;
}
static sftp_handle_t *handle_lookup(const uint8_t *b, int blen, int *po)
{
    if (*po + 4 > blen) return NULL;
    uint32_t n = ssh_get_u32(b + *po); *po += 4;
    if (n != 4 || *po + 4 > blen) { *po += (int)n; return NULL; }
    uint32_t idx = ssh_get_u32(b + *po); *po += 4;
    if (idx >= SFTP_MAX_HANDLES || !handles[idx].used) return NULL;
    return &handles[idx];
}
static int reply_handle(ssh_session_t *s, uint32_t id, int idx)
{
    uint8_t *p = out_buf; int o = 0;
    p[o++] = SSH_FXP_HANDLE;
    ssh_put_u32(p + o, id); o += 4;
    ssh_put_u32(p + o, 4);  o += 4;        /* handle string length = 4 */
    ssh_put_u32(p + o, (uint32_t)idx); o += 4;
    return sftp_send(s, p, o);
}

/* ---- per-request handlers (return 0/-1 for send errors) ---- */

static int do_realpath(ssh_session_t *s, uint32_t id, const uint8_t *b, int bl, int po)
{
    char path[256];
    if (get_pstr(b, bl, &po, path, sizeof(path)) < 0)
        return reply_status(s, id, SSH_FX_FAILURE);
    /* Resolve relative / "." against root (sshd cwd is /). */
    char res[256];
    if (path[0] == 0 || (path[0] == '.' && path[1] == 0)) {
        res[0] = '/'; res[1] = 0;
    } else if (path[0] == '/') {
        snprintf(res, sizeof(res), "%s", path);
    } else {
        snprintf(res, sizeof(res), "/%s", path);
    }
    int rl = (int)strlen(res);
    uint8_t *p = out_buf; int o = 0;
    p[o++] = SSH_FXP_NAME;
    ssh_put_u32(p + o, id); o += 4;
    ssh_put_u32(p + o, 1);  o += 4;          /* count = 1 */
    o = put_str(p, o, res, rl);              /* filename */
    o = put_str(p, o, res, rl);              /* longname */
    ssh_put_u32(p + o, 0); o += 4;           /* attrs: flags = 0 */
    return sftp_send(s, p, o);
}

static int do_stat(ssh_session_t *s, uint32_t id, const uint8_t *b, int bl, int po)
{
    char path[256];
    if (get_pstr(b, bl, &po, path, sizeof(path)) < 0)
        return reply_status(s, id, SSH_FX_FAILURE);
    struct stat st;
    if (stat(path, &st) < 0) return reply_status(s, id, errno_to_fx(errno));
    uint8_t *p = out_buf; int o = 0;
    p[o++] = SSH_FXP_ATTRS;
    ssh_put_u32(p + o, id); o += 4;
    o = put_attrs(p, o, &st);
    return sftp_send(s, p, o);
}

static int do_fstat(ssh_session_t *s, uint32_t id, const uint8_t *b, int bl, int po)
{
    sftp_handle_t *h = handle_lookup(b, bl, &po);
    if (!h || h->is_dir) return reply_status(s, id, SSH_FX_FAILURE);
    struct stat st;
    if (fstat(h->fd, &st) < 0) return reply_status(s, id, errno_to_fx(errno));
    uint8_t *p = out_buf; int o = 0;
    p[o++] = SSH_FXP_ATTRS;
    ssh_put_u32(p + o, id); o += 4;
    o = put_attrs(p, o, &st);
    return sftp_send(s, p, o);
}

static int do_open(ssh_session_t *s, uint32_t id, const uint8_t *b, int bl, int po)
{
    char path[256];
    if (get_pstr(b, bl, &po, path, sizeof(path)) < 0)
        return reply_status(s, id, SSH_FX_FAILURE);
    if (po + 4 > bl) return reply_status(s, id, SSH_FX_FAILURE);
    uint32_t pf = ssh_get_u32(b + po); po += 4;
    /* attrs follow (flags + optional fields) -- parse permissions if present */
    uint32_t mode = 0644;
    if (po + 4 <= bl) {
        uint32_t af = ssh_get_u32(b + po); po += 4;
        if (af & SSH_FILEXFER_ATTR_SIZE) po += 8;
        if (af & SSH_FILEXFER_ATTR_UIDGID) po += 8;
        if (af & SSH_FILEXFER_ATTR_PERMISSIONS) {
            if (po + 4 <= bl) { mode = ssh_get_u32(b + po) & 0777; po += 4; }
        }
    }
    int fl = 0;
    if ((pf & SSH_FXF_READ) && (pf & SSH_FXF_WRITE)) fl = O_RDWR;
    else if (pf & SSH_FXF_WRITE) fl = O_WRONLY;
    else fl = O_RDONLY;
    if (pf & SSH_FXF_CREAT)  fl |= O_CREAT;
    if (pf & SSH_FXF_TRUNC)  fl |= O_TRUNC;
    if (pf & SSH_FXF_APPEND) fl |= O_APPEND;
    if (pf & SSH_FXF_EXCL)   fl |= O_EXCL;

    int fd = open(path, fl, mode);
    if (fd < 0) return reply_status(s, id, errno_to_fx(errno));
    sftp_handle_t *h = handle_alloc();
    if (!h) { close(fd); return reply_status(s, id, SSH_FX_FAILURE); }
    h->is_dir = 0; h->fd = fd;
    snprintf(h->path, sizeof(h->path), "%s", path);
    SSHLOG("[sftp] OPEN %s fl=0x%x -> h%ld\n", path, (unsigned)fl,
           (long)(h - handles));
    return reply_handle(s, id, (int)(h - handles));
}

static int do_read(ssh_session_t *s, uint32_t id, const uint8_t *b, int bl, int po)
{
    sftp_handle_t *h = handle_lookup(b, bl, &po);
    if (!h || h->is_dir) return reply_status(s, id, SSH_FX_FAILURE);
    if (po + 12 > bl) return reply_status(s, id, SSH_FX_FAILURE);
    uint64_t off = get_u64(b + po); po += 8;
    uint32_t want = ssh_get_u32(b + po); po += 4;
    if (want > SFTP_DATAMAX) want = SFTP_DATAMAX;
    long n = pread(h->fd, io_buf, want, (off_t)off);
    if (n < 0) return reply_status(s, id, errno_to_fx(errno));
    if (n == 0) return reply_status(s, id, SSH_FX_EOF);
    uint8_t *p = out_buf; int o = 0;
    p[o++] = SSH_FXP_DATA;
    ssh_put_u32(p + o, id); o += 4;
    ssh_put_u32(p + o, (uint32_t)n); o += 4;
    memcpy(p + o, io_buf, n); o += (int)n;
    return sftp_send(s, p, o);
}

static int do_write(ssh_session_t *s, uint32_t id, const uint8_t *b, int bl, int po)
{
    sftp_handle_t *h = handle_lookup(b, bl, &po);
    if (!h || h->is_dir) return reply_status(s, id, SSH_FX_FAILURE);
    if (po + 12 > bl) return reply_status(s, id, SSH_FX_FAILURE);
    uint64_t off = get_u64(b + po); po += 8;
    uint32_t dlen = ssh_get_u32(b + po); po += 4;
    if (po + (int)dlen > bl) return reply_status(s, id, SSH_FX_FAILURE);
    long total = 0;
    while (total < (long)dlen) {
        long w = pwrite(h->fd, b + po + total, dlen - total, (off_t)(off + total));
        if (w <= 0) return reply_status(s, id, errno_to_fx(errno));
        total += w;
    }
    return reply_status(s, id, SSH_FX_OK);
}

static int do_close(ssh_session_t *s, uint32_t id, const uint8_t *b, int bl, int po)
{
    sftp_handle_t *h = handle_lookup(b, bl, &po);
    if (!h) return reply_status(s, id, SSH_FX_FAILURE);
    if (h->is_dir) { if (h->dir) closedir(h->dir); }
    else { close(h->fd); }
    h->used = 0;
    return reply_status(s, id, SSH_FX_OK);
}

static int do_opendir(ssh_session_t *s, uint32_t id, const uint8_t *b, int bl, int po)
{
    char path[256];
    if (get_pstr(b, bl, &po, path, sizeof(path)) < 0)
        return reply_status(s, id, SSH_FX_FAILURE);
    DIR *d = opendir(path);
    if (!d) return reply_status(s, id, errno_to_fx(errno));
    sftp_handle_t *h = handle_alloc();
    if (!h) { closedir(d); return reply_status(s, id, SSH_FX_FAILURE); }
    h->is_dir = 1; h->dir = d; h->dir_eof = 0;
    snprintf(h->path, sizeof(h->path), "%s", path);
    return reply_handle(s, id, (int)(h - handles));
}

static int do_readdir(ssh_session_t *s, uint32_t id, const uint8_t *b, int bl, int po)
{
    sftp_handle_t *h = handle_lookup(b, bl, &po);
    if (!h || !h->is_dir) return reply_status(s, id, SSH_FX_FAILURE);
    if (h->dir_eof) return reply_status(s, id, SSH_FX_EOF);

    uint8_t *p = out_buf; int o = 0;
    p[o++] = SSH_FXP_NAME;
    ssh_put_u32(p + o, id); o += 4;
    int count_off = o; o += 4;     /* fill count later */
    int count = 0;
    /* Batch up to ~32 entries or until the buffer is comfortably full. */
    while (count < 32 && o < SFTP_MAX - 1024) {
        struct dirent *de = readdir(h->dir);
        if (!de) { h->dir_eof = 1; break; }
        const char *nm = de->d_name;
        char full[512];
        int hl = (int)strlen(h->path);
        /* Avoid a double slash when listing "/" (h->path == "/"). */
        if (hl > 0 && h->path[hl - 1] == '/')
            snprintf(full, sizeof(full), "%s%s", h->path, nm);
        else
            snprintf(full, sizeof(full), "%s/%s", h->path, nm);
        struct stat st;
        if (stat(full, &st) < 0) { memset(&st, 0, sizeof(st)); }
        char ln[340];
        int lnl = build_longname(ln, nm, &st);
        o = put_str(p, o, nm, (int)strlen(nm));
        o = put_str(p, o, ln, lnl);
        o = put_attrs(p, o, &st);
        count++;
    }
    if (count == 0) return reply_status(s, id, SSH_FX_EOF);
    ssh_put_u32(p + count_off, (uint32_t)count);
    return sftp_send(s, p, o);
}

static int do_remove(ssh_session_t *s, uint32_t id, const uint8_t *b, int bl, int po)
{
    char path[256];
    if (get_pstr(b, bl, &po, path, sizeof(path)) < 0)
        return reply_status(s, id, SSH_FX_FAILURE);
    if (unlink(path) < 0) return reply_status(s, id, errno_to_fx(errno));
    return reply_status(s, id, SSH_FX_OK);
}

static int do_mkdir(ssh_session_t *s, uint32_t id, const uint8_t *b, int bl, int po)
{
    char path[256];
    if (get_pstr(b, bl, &po, path, sizeof(path)) < 0)
        return reply_status(s, id, SSH_FX_FAILURE);
    if (mkdir(path, 0755) < 0) return reply_status(s, id, errno_to_fx(errno));
    return reply_status(s, id, SSH_FX_OK);
}

static int do_rmdir(ssh_session_t *s, uint32_t id, const uint8_t *b, int bl, int po)
{
    char path[256];
    if (get_pstr(b, bl, &po, path, sizeof(path)) < 0)
        return reply_status(s, id, SSH_FX_FAILURE);
    if (rmdir(path) < 0) return reply_status(s, id, errno_to_fx(errno));
    return reply_status(s, id, SSH_FX_OK);
}

static int do_rename(ssh_session_t *s, uint32_t id, const uint8_t *b, int bl, int po)
{
    char oldp[256], newp[256];
    if (get_pstr(b, bl, &po, oldp, sizeof(oldp)) < 0) return reply_status(s, id, SSH_FX_FAILURE);
    if (get_pstr(b, bl, &po, newp, sizeof(newp)) < 0) return reply_status(s, id, SSH_FX_FAILURE);
    if (rename(oldp, newp) < 0) return reply_status(s, id, errno_to_fx(errno));
    return reply_status(s, id, SSH_FX_OK);
}

/* SETSTAT/FSETSTAT: honor a size change (truncate); ack everything else. */
static int do_setstat(ssh_session_t *s, uint32_t id, const uint8_t *b, int bl, int po,
                      int is_f)
{
    int fd = -1; char path[256];
    if (is_f) {
        sftp_handle_t *h = handle_lookup(b, bl, &po);
        if (!h || h->is_dir) return reply_status(s, id, SSH_FX_FAILURE);
        fd = h->fd;
    } else {
        if (get_pstr(b, bl, &po, path, sizeof(path)) < 0)
            return reply_status(s, id, SSH_FX_FAILURE);
    }
    if (po + 4 > bl) return reply_status(s, id, SSH_FX_OK);
    uint32_t af = ssh_get_u32(b + po); po += 4;
    if (af & SSH_FILEXFER_ATTR_SIZE) {
        if (po + 8 <= bl) {
            uint64_t sz = get_u64(b + po); po += 8;
            if (is_f) { if (ftruncate(fd, (off_t)sz) < 0)
                            return reply_status(s, id, errno_to_fx(errno)); }
            else      { if (truncate(path, (off_t)sz) < 0)
                            return reply_status(s, id, errno_to_fx(errno)); }
        }
    }
    /* uid/gid/permissions/times: accepted but not applied. */
    return reply_status(s, id, SSH_FX_OK);
}

/* Dispatch one complete SFTP packet body (type + id + rest). */
static int sftp_dispatch(ssh_session_t *s, const uint8_t *b, int bl)
{
    if (bl < 1) return -1;
    uint8_t type = b[0];
    if (type == SSH_FXP_INIT) {
        /* body: type(1) + version(4); reply VERSION(3), no extensions */
        uint8_t *p = out_buf; int o = 0;
        p[o++] = SSH_FXP_VERSION;
        ssh_put_u32(p + o, SFTP_VERSION); o += 4;
        SSHLOG("[sftp] INIT -> VERSION %d\n", SFTP_VERSION);
        return sftp_send(s, p, o);
    }
    if (bl < 5) return -1;
    uint32_t id = ssh_get_u32(b + 1);
    int po = 5;
    switch (type) {
    case SSH_FXP_REALPATH: return do_realpath(s, id, b, bl, po);
    case SSH_FXP_STAT:     return do_stat(s, id, b, bl, po);
    case SSH_FXP_LSTAT:    return do_stat(s, id, b, bl, po);
    case SSH_FXP_FSTAT:    return do_fstat(s, id, b, bl, po);
    case SSH_FXP_OPEN:     return do_open(s, id, b, bl, po);
    case SSH_FXP_READ:     return do_read(s, id, b, bl, po);
    case SSH_FXP_WRITE:    return do_write(s, id, b, bl, po);
    case SSH_FXP_CLOSE:    return do_close(s, id, b, bl, po);
    case SSH_FXP_OPENDIR:  return do_opendir(s, id, b, bl, po);
    case SSH_FXP_READDIR:  return do_readdir(s, id, b, bl, po);
    case SSH_FXP_REMOVE:   return do_remove(s, id, b, bl, po);
    case SSH_FXP_MKDIR:    return do_mkdir(s, id, b, bl, po);
    case SSH_FXP_RMDIR:    return do_rmdir(s, id, b, bl, po);
    case SSH_FXP_RENAME:   return do_rename(s, id, b, bl, po);
    case SSH_FXP_SETSTAT:  return do_setstat(s, id, b, bl, po, 0);
    case SSH_FXP_FSETSTAT: return do_setstat(s, id, b, bl, po, 1);
    default:
        SSHLOG("[sftp] unsupported type %d\n", type);
        return reply_status(s, id, SSH_FX_OP_UNSUPPORTED);
    }
}

/* ----------------------------------------------------------------
 * SFTP subsystem main loop. Reads SSH packets, reassembles SFTP packets from
 * CHANNEL_DATA, dispatches each. Returns 0 on clean end.
 * ---------------------------------------------------------------- */
int ssh_do_sftp(ssh_session_t *s)
{
    sin_len = 0;
    for (int i = 0; i < SFTP_MAX_HANDLES; i++) handles[i].used = 0;
    SSHLOG("[sftp] subsystem started\n");

    uint8_t pkt[SSH_BUF_SIZE];
    int plen;
    int done = 0;

    while (!done) {
        if (ssh_read_packet(s, pkt, &plen) < 0) break;
        if (plen < 1) break;
        uint8_t mtype = pkt[0];

        if (mtype == SSH_MSG_CHANNEL_DATA) {
            int doff = 1 + 4;
            const uint8_t *data; uint32_t dlen;
            if (ssh_get_string(pkt, plen, &doff, &data, &dlen) < 0) continue;
            /* Accumulate into the reassembly buffer. */
            if (sin_len + (int)dlen > SFTP_MAX) {
                SSHLOG("[sftp] reassembly overflow -- abort\n");
                break;
            }
            memcpy(sin_buf + sin_len, data, dlen);
            sin_len += (int)dlen;

            /* Replenish our receive window. */
            s->server_window -= dlen;
            if (s->server_window < SFTP_CHAN_WINDOW / 2) {
                uint8_t wa[16]; int wo = 0;
                wa[wo++] = SSH_MSG_CHANNEL_WINDOW_ADJUST;
                ssh_put_u32(wa + wo, s->client_channel); wo += 4;
                uint32_t adj = SFTP_CHAN_WINDOW - s->server_window;
                ssh_put_u32(wa + wo, adj); wo += 4;
                if (ssh_write_packet(s, wa, wo) < 0) break;
                s->server_window += adj;
            }

            /* Process every complete SFTP packet now buffered. */
            for (;;) {
                if (sin_len < 4) break;
                uint32_t plen2 = ssh_get_u32(sin_buf);
                if (plen2 == 0 || plen2 > SFTP_MAX - 4) { done = 1; break; }
                if (sin_len < 4 + (int)plen2) break;   /* need more */
                if (sftp_dispatch(s, sin_buf + 4, (int)plen2) < 0) { done = 1; break; }
                int consumed = 4 + (int)plen2;
                memmove(sin_buf, sin_buf + consumed, sin_len - consumed);
                sin_len -= consumed;
            }
        } else if (mtype == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
            if (plen >= 9) s->client_window += ssh_get_u32(pkt + 5);
        } else if (mtype == SSH_MSG_CHANNEL_EOF) {
            /* The sftp client sends EOF only after all its requests are done
             * and answered (at `quit`), then waits for OUR EOF before it sends
             * CHANNEL_CLOSE. Finishing here (cleanup sends our EOF+CLOSE)
             * avoids a half-close deadlock that hung the client at exit. */
            done = 1;
        } else if (mtype == SSH_MSG_CHANNEL_CLOSE ||
                   mtype == SSH_MSG_DISCONNECT) {
            done = 1;
        }
    }

    /* Close any open handles. */
    for (int i = 0; i < SFTP_MAX_HANDLES; i++) {
        if (!handles[i].used) continue;
        if (handles[i].is_dir) { if (handles[i].dir) closedir(handles[i].dir); }
        else close(handles[i].fd);
        handles[i].used = 0;
    }
    {
        /* Send exit-status 0 before EOF/CLOSE (RFC 4254 6.10). sftp tolerates
         * its absence, but scp treats a missing exit-status as failure and
         * returns rc=1 even on a perfect transfer. */
        uint8_t es[32]; int so = 0;
        es[so++] = SSH_MSG_CHANNEL_REQUEST;
        ssh_put_u32(es + so, s->client_channel); so += 4;
        ssh_put_string(es, "exit-status", 11, &so);
        es[so++] = 0;                        /* want_reply = FALSE */
        ssh_put_u32(es + so, 0); so += 4;    /* exit status 0 */
        ssh_write_packet(s, es, so);

        uint8_t e[8]; int eo = 0;
        e[eo++] = SSH_MSG_CHANNEL_EOF;
        ssh_put_u32(e + eo, s->client_channel); eo += 4;
        ssh_write_packet(s, e, eo);
        e[0] = SSH_MSG_CHANNEL_CLOSE;
        ssh_write_packet(s, e, eo);
    }
    SSHLOG("[sftp] subsystem ended\n");
    return 0;
}

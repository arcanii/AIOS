/*
 * uk/pal/sel4/tarfs.c -- the Phase C.4 READ-ONLY TARFS: family C (the fs) begins. It serves the
 * embedded aiosroot.tar (a member of the same linked-in CPIO the guests live in) as the guests'
 * filesystem, implementing the fs half of the PAL contract: pal_host_{open,read,lseek,close,fstat,
 * stat}. In-proc as a LIBRARY, never a parked-caller IPC server (the plan's risk R5: the
 * single-threaded kernel must never block on a server); every operation is a bounded walk over
 * immortal image memory -- nothing is copied at mount, a "file" is a pointer+length into the tar.
 *
 * Format: ustar/GNU-tar 512-byte header blocks (name[100] @0, mode octal @100, size octal @124,
 * mtime octal @136, typeflag @156, prefix[155] @345), each followed by size rounded up to 512.
 * mkaiosroot.sh tars from the image's PARENT dir, so every entry carries a top-level prefix
 * directory ("<basename>/..."); tarfs_init derives it from the first entry and lookups strip it.
 * Only typeflags '0'/'\0' (file) and '5' (dir) are served; anything else is skipped over
 * (mkaiosroot cp's everything -- the archive has no links; verified: 42 files + 7 dirs, no pax).
 *
 * Paths: the kernel's read_abspath = read_path + cwd_join WITHOUT path_norm, so guests can hand
 * the PAL "/etc//./motd" -- lookup normalizes textually ("." and empty dropped, ".." popped,
 * clamped at the root) before matching. Write-intent opens (WRONLY/RDWR/CREAT/TRUNC/APPEND) are
 * refused with -EACCES (the fs is read-only; the ABI defines no EROFS). Directories stat but do
 * not open (-EISDIR; getdents is Phase D). Handles are indices into a small table, offset past
 * the console's std handles 0/1/2 (ofile_init_std backs those with pal_host_std(0..2)).
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pal.h"
#include "tarfs.h"
#include "pipe.h"   /* pal_host_read/close route pipe-end handles to the C.5 ring */

/* ---- the mounted archive ---------------------------------------------------------------------- */
static const char   *g_tar;
static unsigned long g_tar_sz;
static char          g_prefix[104];   /* "<topdir>/" -- stripped from every entry name */
static size_t        g_prefix_len;

#define TAR_BLK 512

struct tar_ent {                      /* one resolved entry (a decoded header) */
    const char   *data;
    unsigned long size;
    unsigned int  mode;               /* permission bits from the header (type OR'd in by users) */
    long          mtime;
    int           is_dir;
    unsigned long ino;                /* header offset / 512 + 1 -- stable + unique */
};

static unsigned long tar_oct(const char *p, int n) {
    unsigned long v = 0;
    int i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '0')) i++;         /* leading pad */
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = (v << 3) + (unsigned long)(p[i] - '0');
    return v;
}

/* Normalize an absolute-ish path into "a/b/c" components (no leading slash): empty + "." dropped,
 * ".." popped and clamped at the root. Returns the length, or -1 if it does not fit. */
static long tar_norm(const char *in, char *out, size_t cap) {
    size_t o = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t l = (size_t)(p - seg);
        if (l == 1 && seg[0] == '.') continue;
        if (l == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;                                    /* drop the separator too */
            continue;
        }
        if (o + (o ? 1 : 0) + l >= cap) return -1;
        if (o) out[o++] = '/';
        memcpy(out + o, seg, l);
        o += l;
    }
    out[o] = '\0';
    return (long)o;
}

/* Walk the archive for the entry matching `norm` (a normalized, prefix-less path; "" = the root
 * directory). Returns 0 and fills *e, or -1. */
static int tar_lookup(const char *norm, struct tar_ent *e) {
    if (!g_tar) return -1;
    size_t nlen = strlen(norm);
    unsigned long off = 0;
    while (off + TAR_BLK <= g_tar_sz) {
        const char *h = g_tar + off;
        if (h[0] == '\0') break;                               /* the end-of-archive zero block */
        unsigned long size = tar_oct(h + 124, 12);
        char typ = h[156];
        /* entry name = name[100] (+ ustar prefix[155] if set), minus the archive's top dir */
        char full[260];
        size_t fl = 0;
        if (h[345]) {                                          /* ustar split: prefix "/" name */
            size_t pl = 0; while (pl < 155 && h[345 + pl]) pl++;
            memcpy(full, h + 345, pl); fl = pl;
            full[fl++] = '/';
        }
        { size_t nl = 0; while (nl < 100 && h[nl]) nl++;
          if (fl + nl < sizeof full) { memcpy(full + fl, h, nl); fl += nl; } }
        full[fl] = '\0';
        while (fl && full[fl - 1] == '/') full[--fl] = '\0';   /* dirs carry a trailing slash */
        const char *rel = full;
        if (g_prefix_len && strncmp(full, g_prefix, g_prefix_len) == 0) rel = full + g_prefix_len;
        else if (g_prefix_len && fl + 1 == g_prefix_len && strncmp(full, g_prefix, fl) == 0)
            rel = full + fl;                                   /* the top dir itself == the root */

        if ((typ == '0' || typ == '\0' || typ == '5') &&
            strlen(rel) == nlen && memcmp(rel, norm, nlen) == 0) {
            e->data   = h + TAR_BLK;
            e->size   = size;
            e->mode   = (unsigned int)tar_oct(h + 100, 8) & 07777;
            e->mtime  = (long)tar_oct(h + 136, 12);
            e->is_dir = (typ == '5');
            e->ino    = off / TAR_BLK + 1;
            return 0;
        }
        off += TAR_BLK + ((size + TAR_BLK - 1) & ~(unsigned long)(TAR_BLK - 1));
    }
    return -1;
}

int tarfs_init(const void *base, unsigned long size) {
    g_tar = (const char *)base;
    g_tar_sz = size;
    g_prefix_len = 0;
    if (!g_tar || size < TAR_BLK) { g_tar = NULL; return 0; }
    /* the top-level prefix dir = the first entry's first path component */
    { const char *h = g_tar;
      size_t i = 0;
      while (i < 100 && h[i] && h[i] != '/') i++;
      if (i > 0 && i < sizeof g_prefix - 2) {
          memcpy(g_prefix, h, i);
          g_prefix[i] = '/';
          g_prefix[i + 1] = '\0';
          g_prefix_len = i + 1;
      }
    }
    /* count the regular files (also validates the walk terminates) */
    int nfiles = 0;
    unsigned long off = 0;
    while (off + TAR_BLK <= g_tar_sz) {
        const char *h = g_tar + off;
        if (h[0] == '\0') break;
        unsigned long fsz = tar_oct(h + 124, 12);
        if (h[156] == '0' || h[156] == '\0') nfiles++;
        off += TAR_BLK + ((fsz + TAR_BLK - 1) & ~(unsigned long)(TAR_BLK - 1));
    }
    if (nfiles == 0) g_tar = NULL;                             /* nothing served -> stay -ENOSYS */
    return nfiles;
}

const void *tarfs_find(const char *abspath, unsigned long *size_out) {
    char norm[256];
    struct tar_ent e;
    if (!g_tar || tar_norm(abspath, norm, sizeof norm) < 0) return NULL;
    if (tar_lookup(norm, &e) || e.is_dir) return NULL;
    *size_out = e.size;
    return e.data;
}

/* ---- the open-file table + the PAL fs contract ------------------------------------------------- */
#define TARFS_MAX_OPEN 256            /* == the kernel's MAX_OFILES: the tarfs must never be the
                                       * system's hidden, earlier open-file ceiling */
#define TARFS_FD_BASE  3              /* 0/1/2 are the console's std handles (pal_host_std) */

static struct {
    int            used;
    struct tar_ent e;
    unsigned long  pos;               /* a FILE: byte offset; a DIRECTORY: the getdents logical cursor */
    char           path[256];         /* the normalized path -- a dir handle needs it for *at + getdents */
} g_of[TARFS_MAX_OPEN];

static int of_from_handle(pal_file_t f) {
    long i = (long)f - TARFS_FD_BASE;
    if (i < 0 || i >= TARFS_MAX_OPEN || !g_of[i].used) return -1;
    return (int)i;
}

/* Open an ALREADY-NORMALIZED path (no leading slash, dot segments collapsed). Directories now open
 * (getdents/opendir + fstat need a handle); a subsequent read() on a dir handle -> EISDIR. Shared by
 * pal_host_open (normalizes an abspath) and pal_host_openat (joins a dirfd path first). */
static pal_file_t tarfs_open_norm(const char *norm, uint64_t flags) {
    if ((flags & AIOS_O_ACCMODE) != AIOS_O_RDONLY ||
        (flags & (AIOS_O_CREAT | AIOS_O_TRUNC | AIOS_O_APPEND)))
        return (pal_file_t)-AIOS_EACCES;          /* read-only fs (the ABI defines no EROFS) */
    struct tar_ent e;
    if (norm[0] == '\0') {                         /* the root directory itself ("/") */
        memset(&e, 0, sizeof e); e.is_dir = 1; e.mode = 0755; e.ino = 1;
    } else if (tar_lookup(norm, &e)) {
        return (pal_file_t)-AIOS_ENOENT;
    }
    if ((flags & AIOS_O_DIRECTORY) && !e.is_dir) return (pal_file_t)-AIOS_ENOTDIR;
    for (int i = 0; i < TARFS_MAX_OPEN; i++) {
        if (g_of[i].used) continue;
        g_of[i].used = 1;
        g_of[i].e = e;
        g_of[i].pos = 0;
        size_t k = 0; while (norm[k] && k < sizeof g_of[i].path - 1) { g_of[i].path[k] = norm[k]; k++; }
        g_of[i].path[k] = '\0';
        return (pal_file_t)(TARFS_FD_BASE + i);
    }
    return (pal_file_t)-AIOS_EMFILE;
}

pal_file_t pal_host_open(const char *path, uint64_t flags, uint64_t mode) {
    (void)mode;
    if (!g_tar) return (pal_file_t)-AIOS_ENOSYS;
    char norm[256];
    if (tar_norm(path, norm, sizeof norm) < 0) return (pal_file_t)-AIOS_ENAMETOOLONG;
    return tarfs_open_norm(norm, flags);
}

long pal_host_read(pal_file_t f, void *buf, size_t n) {
    if (pipe_is_handle(f)) return pipe_read(f, buf, n);   /* a pipe read-end (C.5) */
    int i = of_from_handle(f);
    if (i < 0) return -AIOS_ENOSYS;               /* std handles: console read is Phase E */
    if (g_of[i].e.is_dir) return -AIOS_EISDIR;    /* a directory reads via getdents, not read() */
    if (g_of[i].pos >= g_of[i].e.size) return 0;  /* EOF (also covers a past-EOF seek) */
    unsigned long left = g_of[i].e.size - g_of[i].pos;
    if (n > left) n = left;
    memcpy(buf, g_of[i].e.data + g_of[i].pos, n);
    g_of[i].pos += n;
    return (long)n;
}

long long pal_host_lseek(pal_file_t f, long long off, int whence) {
    if (pipe_is_handle(f)) return -AIOS_ESPIPE;   /* a pipe is not seekable (POSIX ESPIPE) */
    int i = of_from_handle(f);
    if (i < 0) return -AIOS_ENOSYS;
    long long base = (whence == AIOS_SEEK_SET) ? 0
                   : (whence == AIOS_SEEK_CUR) ? (long long)g_of[i].pos
                   : (whence == AIOS_SEEK_END) ? (long long)g_of[i].e.size
                   : -1;
    if (base < 0 || off > (long long)((~0ULL >> 1)) - base   /* base+off would overflow (UB) */
        || base + off < 0) return -AIOS_EINVAL;
    g_of[i].pos = (unsigned long)(base + off);    /* past-EOF is legal; reads there return 0 */
    return (long long)g_of[i].pos;
}

int pal_host_close(pal_file_t f) {
    if (pipe_is_handle(f)) return pipe_close(f);  /* a pipe end (C.5): closes an end, may reclaim */
    int i = of_from_handle(f);
    if (i >= 0) g_of[i].used = 0;
    return 0;                                     /* std handles close as a no-op, as before */
}

static void ent_stat(const struct tar_ent *e, struct aios_stat *o) {
    memset(o, 0, sizeof *o);
    o->st_dev     = 0x7a6f;                       /* an arbitrary constant device id ("tar") */
    o->st_ino     = e->ino;
    o->st_mode    = (e->is_dir ? AIOS_S_IFDIR : AIOS_S_IFREG) | e->mode;
    o->st_nlink   = 1;
    o->st_size    = e->is_dir ? 0 : (long long)e->size;
    o->st_blksize = TAR_BLK;
    o->st_blocks  = (long long)((e->size + TAR_BLK - 1) / TAR_BLK);
    o->st_mtim.tv_sec = e->mtime;
    o->st_atim = o->st_mtim;
    o->st_ctim = o->st_mtim;
}

int pal_host_fstat(pal_file_t f, struct aios_stat *o) {
    if (pipe_is_handle(f)) {                       /* a pipe end: a minimal FIFO stat (Linux-parity) */
        memset(o, 0, sizeof *o);
        o->st_mode = AIOS_S_IFIFO | 0600;
        o->st_nlink = 1;
        o->st_blksize = TAR_BLK;
        return 0;
    }
    int i = of_from_handle(f);
    if (i < 0) return -AIOS_ENOSYS;               /* std-handle stat (a tty) is Phase E */
    ent_stat(&g_of[i].e, o);
    return 0;
}

int pal_host_stat(const char *path, struct aios_stat *o, int follow) {
    (void)follow;                                 /* the archive holds no symlinks */
    if (!g_tar) return -AIOS_ENOSYS;
    char norm[256];
    struct tar_ent e;
    if (tar_norm(path, norm, sizeof norm) < 0) return -AIOS_ENAMETOOLONG;
    if (norm[0] == '\0') {                        /* the root dir itself ("/") */
        memset(&e, 0, sizeof e);
        e.is_dir = 1; e.mode = 0755; e.ino = 1;
        ent_stat(&e, o);
        return 0;
    }
    if (tar_lookup(norm, &e)) return -AIOS_ENOENT;
    ent_stat(&e, o);
    return 0;
}

/* ---- directory enumeration (getdents) + the *at family (Phase D breadth) ----------------------- */

/* Fill `name` with the Nth immediate child of the normalized directory `dpath` ("" = the root) in
 * archive order + its ino/type. Returns 0, or -1 when there are fewer than n+1 children. The tar's
 * dirs are all EXPLICIT entries (mkaiosroot cp's a real tree), so every child is a real entry. */
static int dir_nth_child(const char *dpath, int n, char *name, size_t namecap,
                         unsigned long *ino, int *is_dir) {
    size_t dl = strlen(dpath);
    int cnt = 0;
    unsigned long off = 0;
    while (off + TAR_BLK <= g_tar_sz) {
        const char *h = g_tar + off;
        if (h[0] == '\0') break;
        unsigned long size = tar_oct(h + 124, 12);
        char typ = h[156];
        char full[260]; size_t fl = 0;                 /* prefix-stripped rel path (as tar_lookup) */
        if (h[345]) { size_t pl = 0; while (pl < 155 && h[345 + pl]) pl++;
                      memcpy(full, h + 345, pl); fl = pl; full[fl++] = '/'; }
        { size_t nl = 0; while (nl < 100 && h[nl]) nl++;
          if (fl + nl < sizeof full) { memcpy(full + fl, h, nl); fl += nl; } }
        full[fl] = '\0';
        while (fl && full[fl - 1] == '/') full[--fl] = '\0';
        const char *rel = full;
        if (g_prefix_len && strncmp(full, g_prefix, g_prefix_len) == 0) rel = full + g_prefix_len;
        else if (g_prefix_len && fl + 1 == g_prefix_len && strncmp(full, g_prefix, fl) == 0)
            rel = full + fl;                           /* the top prefix dir ITSELF -> the root ""
                                                        * (mirrors tar_lookup -- without this the top
                                                        * entry leaks as a phantom child of "/") */

        const char *child = NULL;                      /* the immediate-child name, or NULL */
        if (dl == 0) { if (rel[0] && !strchr(rel, '/')) child = rel; }
        else if (strncmp(rel, dpath, dl) == 0 && rel[dl] == '/' && rel[dl + 1]
                 && !strchr(rel + dl + 1, '/')) child = rel + dl + 1;

        if (child && (typ == '0' || typ == '\0' || typ == '5')) {
            if (cnt == n) {
                size_t k = 0; while (child[k] && k < namecap - 1) { name[k] = child[k]; k++; }
                name[k] = '\0';
                *ino = off / TAR_BLK + 1;
                *is_dir = (typ == '5');
                return 0;
            }
            cnt++;
        }
        off += TAR_BLK + ((size + TAR_BLK - 1) & ~(unsigned long)(TAR_BLK - 1));
    }
    return -1;
}

long pal_host_getdents(pal_file_t f, void *buf, size_t len) {
    int i = of_from_handle(f);
    if (i < 0) return -AIOS_ENOSYS;
    if (!g_of[i].e.is_dir) return -AIOS_ENOTDIR;
    const size_t HDR = offsetof(struct aios_dirent, d_name);   /* == 19 */
    char *out = (char *)buf;
    size_t used = 0;
    for (;;) {
        long L = (long)g_of[i].pos;                    /* the logical cursor: 0=".", 1="..", 2+=child */
        char name[128]; unsigned long ino = 1; int is_dir = 1;
        if      (L == 0) { name[0] = '.'; name[1] = '\0'; ino = g_of[i].e.ino ? g_of[i].e.ino : 1; }
        else if (L == 1) { name[0] = name[1] = '.'; name[2] = '\0'; }
        else if (dir_nth_child(g_of[i].path, (int)(L - 2), name, sizeof name, &ino, &is_dir) != 0) break;
        size_t nl = 0; while (name[nl]) nl++;
        size_t reclen = (HDR + nl + 1 + 7) & ~(size_t)7;           /* 8-align (Linux dirent64 does) */
        if (used + reclen > len) { if (used == 0) return -AIOS_EINVAL; break; }   /* buffer full */
        struct aios_dirent *d = (struct aios_dirent *)(out + used);
        d->d_ino    = ino;
        d->d_off    = L + 1;
        d->d_reclen = (unsigned short)reclen;
        d->d_type   = (unsigned char)(is_dir ? AIOS_DT_DIR : AIOS_DT_REG);
        memcpy(d->d_name, name, nl);
        memset(d->d_name + nl, 0, reclen - HDR - nl);              /* NUL + alignment padding */
        used += reclen;
        g_of[i].pos = (unsigned long)(L + 1);
    }
    return (long)used;                                            /* 0 = end of directory */
}

/* Resolve an *at (dir, path) pair to a normalized tarfs path. AT_FDCWD (the kernel already joined the
 * cwd) or an absolute path -> normalize as-is; a real dir handle -> join its stored path + the rel. */
static long at_resolve(pal_file_t dir, const char *path, char *norm, size_t cap) {
    if (dir == PAL_AT_FDCWD || path[0] == '/')
        return tar_norm(path, norm, cap) < 0 ? -AIOS_ENAMETOOLONG : 0;
    int i = of_from_handle(dir);
    if (i < 0) return -AIOS_EBADF;
    if (!g_of[i].e.is_dir) return -AIOS_ENOTDIR;
    char joined[512]; size_t o = 0;
    const char *dp = g_of[i].path;
    while (dp[o] && o < sizeof joined - 2) { joined[o] = dp[o]; o++; }
    joined[o++] = '/';
    size_t j = 0; while (path[j] && o < sizeof joined - 1) joined[o++] = path[j++];
    joined[o] = '\0';
    return tar_norm(joined, norm, cap) < 0 ? -AIOS_ENAMETOOLONG : 0;
}

pal_file_t pal_host_openat(pal_file_t dir, const char *path, uint64_t flags, uint64_t mode) {
    (void)mode;
    if (!g_tar) return (pal_file_t)-AIOS_ENOSYS;
    char norm[256];
    long e = at_resolve(dir, path, norm, sizeof norm);
    if (e) return (pal_file_t)e;
    return tarfs_open_norm(norm, flags);
}

int pal_host_fstatat(pal_file_t dir, const char *path, struct aios_stat *o, int follow) {
    (void)follow;
    if (!g_tar) return -AIOS_ENOSYS;
    char norm[256];
    long e = at_resolve(dir, path, norm, sizeof norm);
    if (e) return (int)e;
    struct tar_ent ent;
    if (norm[0] == '\0') { memset(&ent, 0, sizeof ent); ent.is_dir = 1; ent.mode = 0755; ent.ino = 1; }
    else if (tar_lookup(norm, &ent)) return -AIOS_ENOENT;
    ent_stat(&ent, o);
    return 0;
}

int pal_host_faccessat(pal_file_t dir, const char *path, int amode) {
    if (!g_tar) return -AIOS_ENOSYS;
    char norm[256];
    long e = at_resolve(dir, path, norm, sizeof norm);
    if (e) return (int)e;
    struct tar_ent ent;
    if (norm[0] != '\0' && tar_lookup(norm, &ent)) return -AIOS_ENOENT;   /* root always exists */
    if (amode & AIOS_W_OK) return -AIOS_EACCES;   /* the tarfs is read-only */
    return 0;                                     /* exists; R_OK/X_OK/F_OK granted */
}

long pal_host_readlink(const char *path, char *buf, size_t bufsiz) {
    (void)buf; (void)bufsiz;
    if (!g_tar) return -AIOS_ENOSYS;
    char norm[256];
    struct tar_ent ent;
    if (tar_norm(path, norm, sizeof norm) < 0) return -AIOS_ENAMETOOLONG;
    if (norm[0] == '\0') return -AIOS_EINVAL;                 /* the root is not a symlink */
    if (tar_lookup(norm, &ent)) return -AIOS_ENOENT;
    return -AIOS_EINVAL;                                      /* the archive holds no symlinks */
}

int pal_host_chdir(const char *path) {
    if (!g_tar) return -AIOS_ENOSYS;
    char norm[256];
    struct tar_ent ent;
    if (tar_norm(path, norm, sizeof norm) < 0) return -AIOS_ENAMETOOLONG;
    if (norm[0] == '\0') return 0;                            /* "/" is a directory */
    if (tar_lookup(norm, &ent)) return -AIOS_ENOENT;
    if (!ent.is_dir) return -AIOS_ENOTDIR;
    return 0;                                                 /* the kernel records the cwd string */
}

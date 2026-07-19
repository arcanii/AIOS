/*
 * uk/pal/sel4/tarfs.c -- the AIOS-on-seL4 filesystem: a WRITABLE in-memory fs SEEDED from the
 * embedded aiosroot.tar. Family C's fs half of the PAL contract lives here (pal_host_{open,read,
 * write,lseek,close,fstat,stat,getdents,openat,fstatat,faccessat,readlink,chdir,mkdir,rmdir,unlink,
 * rename,unlinkat,fchmodat,utimensat}). In-proc as a LIBRARY, never a parked-caller IPC server (the
 * plan's risk R5: the single-threaded kernel must never block on a server).
 *
 * Phase C.4 served the tar READ-ONLY by re-walking it per lookup. Phase D.3 (this) turns it into a
 * real writable fs the way the plan called for ("a writable RAM-fs from aiosroot.tar") -- WITHOUT
 * copying 3 MB at boot:
 *
 *   - MOUNT parses the tar ONCE into an fsnode TREE (a directory is a sibling list of children).
 *     Each regular file's bytes are NOT copied: the node points at the tar image itself (node->ro),
 *     which lives in immortal .rodata. So the whole userland costs only ~49 nodes of metadata.
 *   - WRITING a file COPIES IT UP on demand (node_ensure_rw): the first write allocates a heap
 *     buffer (the root task has a 6 MB muslc morecore, settings-uk.cmake), copies the tar bytes in,
 *     and the node becomes heap-backed. Files created at runtime are heap-backed from birth. So a
 *     read-only workload allocates nothing, and `echo hi > /tmp/x` costs one small buffer.
 *   - The TREE is the single source of truth after mount: create/unlink/mkdir/rmdir/rename are
 *     ordinary tree edits. There is no union/whiteout layering to reason about.
 *
 * The tar is still the seed format: ustar/GNU 512-byte header blocks (name[100] @0, mode @100,
 * size @124, mtime @136, typeflag @156, prefix[155] @345), each followed by size rounded up to 512.
 * mkaiosroot.sh tars from the image's PARENT dir, so every entry carries a top-level prefix
 * directory ("<basename>/..."); mount derives it from the first entry and strips it (the entry for
 * the top dir ITSELF maps to the root). Only typeflags '0'/'\0' (file) and '5' (dir) are seeded.
 *
 * Paths: the kernel's read_abspath = read_path + cwd_join WITHOUT path_norm, so guests can hand the
 * PAL "/etc//./motd" -- fs_norm() normalizes textually ("." and empty dropped, ".." popped, clamped
 * at the root) before every lookup. Handles are indices into a small table, offset past the
 * console's std handles 0/1/2; pipe-end handles (pipe.c) are routed out at the top of each op.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>              /* malloc/realloc/free -- the root task's 6 MB muslc morecore */

#include "pal.h"
#include "tarfs.h"
#include "pipe.h"   /* pal_host_read/write/close route pipe-end handles to the C.5 ring */

#define TAR_BLK       512
#define FS_NAME_MAX    96        /* one path component */
#define FS_PATH_MAX   256        /* a whole normalized path */

/* ---- the node tree ----------------------------------------------------------------------------- */
typedef struct fsnode {
    char           name[FS_NAME_MAX];   /* basename; "" for the root */
    int            is_dir;
    unsigned int   mode;                /* permission bits (type comes from is_dir) */
    long           mtime;
    unsigned long  ino;
    const unsigned char *ro;            /* tar-backed bytes (immortal); NULL once heap-backed */
    unsigned char *rw;                  /* heap buffer; NULL until the first write */
    unsigned long  size;                /* logical size */
    unsigned long  cap;                 /* rw capacity */
    unsigned long  dseq;                /* MONOTONIC directory sequence, assigned on every dir_link.
                                         * getdents cursors store "the next dseq to emit" instead of a
                                         * positional index, so unlinking an already-listed sibling
                                         * cannot shift the cursor and skip an entry. */
    struct fsnode *parent, *child, *sibling;
} fsnode_t;

static fsnode_t     *g_root;
static unsigned long g_next_ino = 1;
static unsigned long g_next_dseq = 1;   /* never reused; a re-link gets a fresh (higher) dseq */
static int           g_nfiles;          /* regular files seeded (0 = nothing mounted) */

static unsigned long tar_oct(const char *p, int n) {
    unsigned long v = 0;
    int i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '0')) i++;          /* leading pad */
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = (v << 3) + (unsigned long)(p[i] - '0');
    return v;
}

/* Normalize an absolute-ish path into "a/b/c" (no leading slash): empty + "." dropped, ".." popped
 * and clamped at the root. Returns the length, or -1 if it does not fit. */
static long fs_norm(const char *in, char *out, size_t cap) {
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
            if (o > 0) o--;                                     /* drop the separator too */
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

static fsnode_t *node_new(const char *name, int is_dir, unsigned int mode) {
    fsnode_t *n = (fsnode_t *)malloc(sizeof *n);
    if (!n) return NULL;
    memset(n, 0, sizeof *n);
    size_t k = 0; while (name[k] && k < sizeof n->name - 1) { n->name[k] = name[k]; k++; }
    n->name[k] = '\0';
    n->is_dir = is_dir;
    n->mode   = mode;
    n->ino    = g_next_ino++;
    return n;
}

static fsnode_t *dir_find_child(fsnode_t *d, const char *name, size_t len) {
    if (!d || !d->is_dir) return NULL;
    for (fsnode_t *c = d->child; c; c = c->sibling)
        if (strlen(c->name) == len && memcmp(c->name, name, len) == 0) return c;
    return NULL;
}

static void dir_link(fsnode_t *d, fsnode_t *n) {   /* append, so the list stays sorted by dseq */
    n->parent = d;
    n->sibling = NULL;
    n->dseq = g_next_dseq++;

    if (!d->child) { d->child = n; return; }
    fsnode_t *t = d->child;
    while (t->sibling) t = t->sibling;
    t->sibling = n;
}

static void dir_unlink(fsnode_t *d, fsnode_t *n) {
    if (!d || d->child == NULL) return;
    if (d->child == n) { d->child = n->sibling; n->sibling = NULL; n->parent = NULL; return; }
    for (fsnode_t *c = d->child; c->sibling; c = c->sibling)
        if (c->sibling == n) { c->sibling = n->sibling; n->sibling = NULL; n->parent = NULL; return; }
}

static void node_free(fsnode_t *n) {
    if (!n) return;
    if (n->rw) free(n->rw);
    free(n);
}

/* Resolve a NORMALIZED path to its node ("" = the root). NULL if any component is missing or a
 * non-final component is not a directory. */
static fsnode_t *fs_lookup(const char *norm) {
    if (!g_root) return NULL;
    fsnode_t *cur = g_root;
    const char *p = norm;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t l = (size_t)(p - seg);
        if (!cur->is_dir) return NULL;
        cur = dir_find_child(cur, seg, l);
        if (!cur) return NULL;
        if (*p == '/') p++;
    }
    return cur;
}

/* Resolve the PARENT directory of a normalized path, handing back the final component. NULL if the
 * parent is missing / not a directory, or the path is the root itself (no parent). */
static fsnode_t *fs_lookup_parent(const char *norm, const char **base_out, size_t *baselen_out) {
    if (!g_root || norm[0] == '\0') return NULL;
    const char *slash = NULL;
    for (const char *p = norm; *p; p++) if (*p == '/') slash = p;
    fsnode_t *d;
    if (!slash) { d = g_root; *base_out = norm; *baselen_out = strlen(norm); }
    else {
        char pdir[FS_PATH_MAX];
        size_t pl = (size_t)(slash - norm);
        if (pl >= sizeof pdir) return NULL;
        memcpy(pdir, norm, pl); pdir[pl] = '\0';
        d = fs_lookup(pdir);
        *base_out = slash + 1;
        *baselen_out = strlen(slash + 1);
    }
    if (!d || !d->is_dir) return NULL;
    return d;
}

/* Make `n` heap-backed with at least `need` bytes of capacity, copying up any existing content
 * (the tar bytes on the first write). Returns 0, or -1 on allocation failure. */
static int node_ensure_rw(fsnode_t *n, unsigned long need) {
    if (n->rw && n->cap >= need) return 0;
    /* The whole EXISTING file must survive a copy-up, even when this write only touches a prefix:
     * a partial in-place write (O_RDWR, no O_TRUNC) has need < size, and clamping to `need` would
     * both destroy the tail and leave size > cap -- every later read would then memcpy past the
     * buffer. Grow to at least the live size, and copy all of it. (A caller that INTENDS to discard
     * the content -- O_TRUNC -- zeroes n->size first, so `keep` is 0 and nothing is copied.) */
    if (need < n->size) need = n->size;
    unsigned long cap = n->cap ? n->cap : 64;
    while (cap < need) {
        if (cap > (unsigned long)1 << 30) return -1;
        cap *= 2;
    }
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) return -1;
    unsigned long keep = n->size;                       /* cap >= need >= size, so this always fits */
    if (keep) {
        const unsigned char *src = n->rw ? n->rw : n->ro;
        if (src) memcpy(buf, src, keep);
        else keep = 0;
    }
    if (keep < cap) memset(buf + keep, 0, cap - keep);
    if (n->rw) free(n->rw);
    n->rw = buf;
    n->cap = cap;
    n->ro = NULL;                       /* fully heap-backed now */
    return 0;
}

/* ---- mount: seed the tree from the tar ---------------------------------------------------------- */

/* mkdir -p the directory components of a seed path, returning the deepest directory. */
static fsnode_t *seed_dirs(const char *rel, size_t len) {
    fsnode_t *cur = g_root;
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && rel[j] != '/') j++;
        if (j > i) {
            fsnode_t *c = dir_find_child(cur, rel + i, j - i);
            if (!c) {
                char nm[FS_NAME_MAX];
                size_t l = j - i; if (l >= sizeof nm) l = sizeof nm - 1;
                memcpy(nm, rel + i, l); nm[l] = '\0';
                c = node_new(nm, 1, 0755);
                if (!c) return NULL;
                dir_link(cur, c);
            }
            if (!c->is_dir) return NULL;
            cur = c;
        }
        i = (j < len) ? j + 1 : j;
    }
    return cur;
}

int tarfs_init(const void *base, unsigned long size) {
    const char *tar = (const char *)base;
    g_root = node_new("", 1, 0755);
    g_nfiles = 0;
    if (!g_root || !tar || size < TAR_BLK) { g_root = NULL; return 0; }

    /* the top-level prefix dir = the first entry's first path component */
    char prefix[FS_NAME_MAX + 2]; size_t prefix_len = 0;
    { size_t i = 0;
      while (i < 100 && tar[i] && tar[i] != '/') i++;
      if (i > 0 && i < sizeof prefix - 2) {
          memcpy(prefix, tar, i); prefix[i] = '/'; prefix[i + 1] = '\0';
          prefix_len = i + 1;
      } else prefix[0] = '\0';
    }

    unsigned long off = 0;
    while (off + TAR_BLK <= size) {
        const char *h = tar + off;
        if (h[0] == '\0') break;                            /* end-of-archive block */
        unsigned long fsz = tar_oct(h + 124, 12);
        char typ = h[156];

        char full[FS_PATH_MAX + 4]; size_t fl = 0;          /* name (+ ustar prefix) */
        if (h[345]) { size_t pl = 0; while (pl < 155 && h[345 + pl]) pl++;
                      if (pl < sizeof full - 2) { memcpy(full, h + 345, pl); fl = pl; full[fl++] = '/'; } }
        { size_t nl = 0; while (nl < 100 && h[nl]) nl++;
          if (fl + nl < sizeof full) { memcpy(full + fl, h, nl); fl += nl; } }
        full[fl] = '\0';
        while (fl && full[fl - 1] == '/') full[--fl] = '\0';  /* dirs carry a trailing slash */

        const char *rel = full;                              /* strip the archive's top dir */
        if (prefix_len && strncmp(full, prefix, prefix_len) == 0) rel = full + prefix_len;
        else if (prefix_len && fl + 1 == prefix_len && strncmp(full, prefix, fl) == 0)
            rel = full + fl;                                 /* the top dir ITSELF -> the root */

        if (rel[0] && (typ == '0' || typ == '\0' || typ == '5')) {
            size_t rl = strlen(rel);
            const char *slash = NULL;
            for (const char *p = rel; *p; p++) if (*p == '/') slash = p;
            fsnode_t *d = slash ? seed_dirs(rel, (size_t)(slash - rel)) : g_root;
            const char *bn = slash ? slash + 1 : rel;
            size_t bl = slash ? rl - (size_t)(slash + 1 - rel) : rl;
            if (d && bl && !dir_find_child(d, bn, bl)) {
                char nm[FS_NAME_MAX];
                size_t l = bl; if (l >= sizeof nm) l = sizeof nm - 1;
                memcpy(nm, bn, l); nm[l] = '\0';
                fsnode_t *n = node_new(nm, typ == '5', (unsigned int)tar_oct(h + 100, 8) & 07777);
                if (n) {
                    n->mtime = (long)tar_oct(h + 136, 12);
                    if (!n->is_dir) {
                        n->ro = (const unsigned char *)(h + TAR_BLK);   /* immortal tar bytes: no copy */
                        n->size = fsz;
                        g_nfiles++;
                    }
                    dir_link(d, n);
                }
            }
        }
        off += TAR_BLK + ((fsz + TAR_BLK - 1) & ~(unsigned long)(TAR_BLK - 1));
    }
    if (g_nfiles == 0) { g_root = NULL; }                    /* nothing served -> stay -ENOSYS */
    return g_nfiles;
}

const void *tarfs_find(const char *abspath, unsigned long *size_out) {
    char norm[FS_PATH_MAX];
    if (!g_root || fs_norm(abspath, norm, sizeof norm) < 0) return NULL;
    fsnode_t *n = fs_lookup(norm);
    if (!n || n->is_dir) return NULL;
    *size_out = n->size;
    return n->rw ? (const void *)n->rw : (const void *)n->ro;   /* exec loads whichever backs it */
}

/* ---- the open-file table ------------------------------------------------------------------------ */
#define TARFS_MAX_OPEN 256            /* == the kernel's MAX_OFILES */
#define TARFS_FD_BASE  3              /* 0/1/2 are the console's std handles (pal_host_std) */

static struct {
    int            used;
    fsnode_t      *n;
    unsigned long  pos;               /* a FILE: byte offset; a DIRECTORY: 0=".", 1="..", 2=children */
    unsigned long  dseq;              /* a DIRECTORY: the next child dseq to emit (mutation-stable) */
    int            writable, append;
    char           path[FS_PATH_MAX]; /* normalized path -- a dir handle needs it for the *at family */
} g_of[TARFS_MAX_OPEN];

static int of_from_handle(pal_file_t f) {
    long i = (long)f - TARFS_FD_BASE;
    if (i < 0 || i >= TARFS_MAX_OPEN || !g_of[i].used) return -1;
    return (int)i;
}

/* Open an ALREADY-NORMALIZED path. Handles O_CREAT/O_TRUNC/O_APPEND and the write modes; a
 * directory opens read-only (getdents/fstat), and read() on it -> EISDIR. */
static pal_file_t tarfs_open_norm(const char *norm, uint64_t flags, uint64_t mode) {
    int acc = (int)(flags & AIOS_O_ACCMODE);
    int want_write = (acc == AIOS_O_WRONLY || acc == AIOS_O_RDWR) ||
                     (flags & (AIOS_O_TRUNC | AIOS_O_APPEND));
    fsnode_t *n = fs_lookup(norm);

    if (!n) {
        if (!(flags & AIOS_O_CREAT)) return (pal_file_t)-AIOS_ENOENT;
        const char *bn; size_t bl;
        fsnode_t *d = fs_lookup_parent(norm, &bn, &bl);
        if (!d) return (pal_file_t)-AIOS_ENOENT;
        if (bl == 0 || bl >= FS_NAME_MAX) return (pal_file_t)-AIOS_ENAMETOOLONG;
        char nm[FS_NAME_MAX]; memcpy(nm, bn, bl); nm[bl] = '\0';
        n = node_new(nm, 0, (unsigned int)mode & 07777);
        if (!n) return (pal_file_t)-AIOS_ENOMEM;
        dir_link(d, n);
    } else {
        if ((flags & AIOS_O_DIRECTORY) && !n->is_dir) return (pal_file_t)-AIOS_ENOTDIR;
        if (n->is_dir && want_write) return (pal_file_t)-AIOS_EISDIR;
        if ((flags & AIOS_O_CREAT) && (flags & AIOS_O_EXCL)) return (pal_file_t)-AIOS_EEXIST;
        if ((flags & AIOS_O_TRUNC) && !n->is_dir) {         /* truncate to empty */
            n->size = 0;                                    /* BEFORE the copy-up: discard, don't copy */
            if (node_ensure_rw(n, 1) != 0) return (pal_file_t)-AIOS_ENOMEM;
        }
    }

    for (int i = 0; i < TARFS_MAX_OPEN; i++) {
        if (g_of[i].used) continue;
        g_of[i].used = 1;
        g_of[i].n = n;
        g_of[i].pos = 0;
        g_of[i].dseq = 0;
        g_of[i].writable = want_write && !n->is_dir;
        g_of[i].append = (flags & AIOS_O_APPEND) ? 1 : 0;
        size_t k = 0; while (norm[k] && k < sizeof g_of[i].path - 1) { g_of[i].path[k] = norm[k]; k++; }
        g_of[i].path[k] = '\0';
        return (pal_file_t)(TARFS_FD_BASE + i);
    }
    return (pal_file_t)-AIOS_EMFILE;
}

pal_file_t pal_host_open(const char *path, uint64_t flags, uint64_t mode) {
    if (!g_root) return (pal_file_t)-AIOS_ENOSYS;
    char norm[FS_PATH_MAX];
    if (fs_norm(path, norm, sizeof norm) < 0) return (pal_file_t)-AIOS_ENAMETOOLONG;
    if (norm[0] == '\0') {                                   /* "/" -- the root directory */
        if ((flags & AIOS_O_ACCMODE) != AIOS_O_RDONLY) return (pal_file_t)-AIOS_EISDIR;
    }
    return tarfs_open_norm(norm, flags, mode);
}

long pal_host_read(pal_file_t f, void *buf, size_t n) {
    if (pipe_is_handle(f)) return pipe_read(f, buf, n);       /* a pipe read-end (C.5) */
    int i = of_from_handle(f);
    if (i < 0) return -AIOS_ENOSYS;               /* std handles: console read is Phase E */
    fsnode_t *nd = g_of[i].n;
    if (nd->is_dir) return -AIOS_EISDIR;          /* a directory reads via getdents */
    if (g_of[i].pos >= nd->size) return 0;        /* EOF (also covers a past-EOF seek) */
    unsigned long left = nd->size - g_of[i].pos;
    if (n > left) n = left;
    const unsigned char *src = nd->rw ? nd->rw : nd->ro;
    if (!src) return 0;
    memcpy(buf, src + g_of[i].pos, n);
    g_of[i].pos += n;
    return (long)n;
}

/* The fs half of pal_host_write: boot.c owns that symbol (console 0/1/2 + pipe routing) and calls
 * this for everything else. Returns bytes written, or a negative AIOS errno. */
long tarfs_write(pal_file_t f, const void *buf, size_t n) {
    int i = of_from_handle(f);
    if (i < 0) return -AIOS_ENOSYS;
    fsnode_t *nd = g_of[i].n;
    if (nd->is_dir) return -AIOS_EISDIR;
    if (!g_of[i].writable) return -AIOS_EBADF;    /* opened read-only */
    if (n == 0) return 0;
    unsigned long at = g_of[i].append ? nd->size : g_of[i].pos;
    unsigned long end = at + n;
    if (end < at) return -AIOS_EINVAL;            /* overflow */
    if (node_ensure_rw(nd, end) != 0) return -AIOS_ENOMEM;
    if (at > nd->size) memset(nd->rw + nd->size, 0, at - nd->size);   /* a seek-past-EOF hole */
    memcpy(nd->rw + at, buf, n);
    if (end > nd->size) nd->size = end;
    g_of[i].pos = end;
    return (long)n;
}

long long pal_host_lseek(pal_file_t f, long long off, int whence) {
    if (pipe_is_handle(f)) return -AIOS_ESPIPE;   /* a pipe is not seekable (POSIX) */
    int i = of_from_handle(f);
    if (i < 0) return -AIOS_ENOSYS;
    long long base = (whence == AIOS_SEEK_SET) ? 0
                   : (whence == AIOS_SEEK_CUR) ? (long long)g_of[i].pos
                   : (whence == AIOS_SEEK_END) ? (long long)g_of[i].n->size
                   : -1;
    if (base < 0 || off > (long long)((~0ULL >> 1)) - base   /* base+off would overflow (UB) */
        || base + off < 0) return -AIOS_EINVAL;
    g_of[i].pos = (unsigned long)(base + off);    /* past-EOF is legal; reads there return 0 */
    return (long long)g_of[i].pos;
}

int pal_host_close(pal_file_t f) {
    if (pipe_is_handle(f)) return pipe_close(f);  /* a pipe end (C.5) */
    int i = of_from_handle(f);
    if (i >= 0) g_of[i].used = 0;
    return 0;                                     /* std handles close as a no-op, as before */
}

static void node_stat(const fsnode_t *n, struct aios_stat *o) {
    memset(o, 0, sizeof *o);
    o->st_dev     = 0x7a6f;                       /* an arbitrary constant device id ("tar") */
    o->st_ino     = n->ino;
    o->st_mode    = (n->is_dir ? AIOS_S_IFDIR : AIOS_S_IFREG) | n->mode;
    o->st_nlink   = 1;
    o->st_size    = n->is_dir ? 0 : (long long)n->size;
    o->st_blksize = TAR_BLK;
    o->st_blocks  = (long long)((n->size + TAR_BLK - 1) / TAR_BLK);
    o->st_mtim.tv_sec = n->mtime;
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
    node_stat(g_of[i].n, o);
    return 0;
}

int pal_host_stat(const char *path, struct aios_stat *o, int follow) {
    (void)follow;                                 /* the tree holds no symlinks */
    if (!g_root) return -AIOS_ENOSYS;
    char norm[FS_PATH_MAX];
    if (fs_norm(path, norm, sizeof norm) < 0) return -AIOS_ENAMETOOLONG;
    fsnode_t *n = fs_lookup(norm);
    if (!n) return -AIOS_ENOENT;
    node_stat(n, o);
    return 0;
}

/* ---- directory enumeration (getdents) ----------------------------------------------------------- */
long pal_host_getdents(pal_file_t f, void *buf, size_t len) {
    int i = of_from_handle(f);
    if (i < 0) return -AIOS_ENOSYS;
    fsnode_t *d = g_of[i].n;
    if (!d->is_dir) return -AIOS_ENOTDIR;
    const size_t HDR = offsetof(struct aios_dirent, d_name);   /* == 19 */
    char *out = (char *)buf;
    size_t used = 0;
    for (;;) {
        long L = (long)g_of[i].pos;                /* phase: 0=".", 1="..", 2=children (by dseq) */
        const char *nm; unsigned long ino; int isdir;
        unsigned long next_dseq = g_of[i].dseq;
        if      (L == 0) { nm = ".";  ino = d->ino;                         isdir = 1; }
        else if (L == 1) { nm = ".."; ino = d->parent ? d->parent->ino : d->ino; isdir = 1; }
        else {
            /* Children are emitted in dseq order (the list is sorted because dir_link appends with an
             * ever-increasing dseq). Skipping entries already emitted by dseq -- rather than counting
             * position -- makes enumeration stable while the directory MUTATES: unlinking a listed
             * sibling shifts no cursor, and a fresh create lands at a higher dseq (so it is listed
             * later, or not at all if enumeration already passed it -- both POSIX-legal). */
            fsnode_t *c = d->child;
            while (c && c->dseq < g_of[i].dseq) c = c->sibling;
            if (!c) break;                          /* end of directory */
            nm = c->name; ino = c->ino; isdir = c->is_dir;
            next_dseq = c->dseq + 1;
        }
        size_t nl = strlen(nm);
        size_t reclen = (HDR + nl + 1 + 7) & ~(size_t)7;        /* 8-align (Linux dirent64 does) */
        if (used + reclen > len) { if (used == 0) return -AIOS_EINVAL; break; }   /* buffer full */
        struct aios_dirent *e = (struct aios_dirent *)(out + used);
        e->d_ino    = ino;
        e->d_off    = (long long)next_dseq;   /* an opaque resume cookie */
        e->d_reclen = (unsigned short)reclen;
        e->d_type   = (unsigned char)(isdir ? AIOS_DT_DIR : AIOS_DT_REG);
        memcpy(e->d_name, nm, nl);
        memset(e->d_name + nl, 0, reclen - HDR - nl);           /* NUL + alignment padding */
        used += reclen;
        g_of[i].pos = (L < 2) ? (unsigned long)(L + 1) : 2;      /* stay in the children phase */
        g_of[i].dseq = next_dseq;
    }
    return (long)used;                                          /* 0 = end of directory */
}

/* ---- the *at family + the mutating ops ---------------------------------------------------------- */

/* Resolve an *at (dir, path) pair to a normalized path. AT_FDCWD (the kernel already joined the cwd)
 * or an absolute path -> normalize as-is; a real dir handle -> join its stored path + the rel. */
static long at_resolve(pal_file_t dir, const char *path, char *norm, size_t cap) {
    if (dir == PAL_AT_FDCWD || path[0] == '/')
        return fs_norm(path, norm, cap) < 0 ? -AIOS_ENAMETOOLONG : 0;
    int i = of_from_handle(dir);
    if (i < 0) return -AIOS_EBADF;
    if (!g_of[i].n->is_dir) return -AIOS_ENOTDIR;
    char joined[FS_PATH_MAX * 2]; size_t o = 0;
    const char *dp = g_of[i].path;
    while (dp[o] && o < sizeof joined - 2) { joined[o] = dp[o]; o++; }
    joined[o++] = '/';
    size_t j = 0; while (path[j] && o < sizeof joined - 1) joined[o++] = path[j++];
    joined[o] = '\0';
    return fs_norm(joined, norm, cap) < 0 ? -AIOS_ENAMETOOLONG : 0;
}

pal_file_t pal_host_openat(pal_file_t dir, const char *path, uint64_t flags, uint64_t mode) {
    if (!g_root) return (pal_file_t)-AIOS_ENOSYS;
    char norm[FS_PATH_MAX];
    long e = at_resolve(dir, path, norm, sizeof norm);
    if (e) return (pal_file_t)e;
    return tarfs_open_norm(norm, flags, mode);
}

int pal_host_fstatat(pal_file_t dir, const char *path, struct aios_stat *o, int follow) {
    (void)follow;
    if (!g_root) return -AIOS_ENOSYS;
    char norm[FS_PATH_MAX];
    long e = at_resolve(dir, path, norm, sizeof norm);
    if (e) return (int)e;
    fsnode_t *n = fs_lookup(norm);
    if (!n) return -AIOS_ENOENT;
    node_stat(n, o);
    return 0;
}

int pal_host_faccessat(pal_file_t dir, const char *path, int amode) {
    if (!g_root) return -AIOS_ENOSYS;
    char norm[FS_PATH_MAX];
    long e = at_resolve(dir, path, norm, sizeof norm);
    if (e) return (int)e;
    if (!fs_lookup(norm)) return -AIOS_ENOENT;
    return 0;                                     /* the whole tree is now readable + writable */
}

long pal_host_readlink(const char *path, char *buf, size_t bufsiz) {
    (void)buf; (void)bufsiz;
    if (!g_root) return -AIOS_ENOSYS;
    char norm[FS_PATH_MAX];
    if (fs_norm(path, norm, sizeof norm) < 0) return -AIOS_ENAMETOOLONG;
    if (!fs_lookup(norm)) return -AIOS_ENOENT;
    return -AIOS_EINVAL;                          /* the tree holds no symlinks */
}

int pal_host_chdir(const char *path) {
    if (!g_root) return -AIOS_ENOSYS;
    char norm[FS_PATH_MAX];
    if (fs_norm(path, norm, sizeof norm) < 0) return -AIOS_ENAMETOOLONG;
    fsnode_t *n = fs_lookup(norm);
    if (!n) return -AIOS_ENOENT;
    if (!n->is_dir) return -AIOS_ENOTDIR;
    return 0;                                     /* the kernel records the cwd string */
}

/* Create a directory at a normalized path. */
static int fs_mkdir_norm(const char *norm, unsigned int mode) {
    if (norm[0] == '\0') return -AIOS_EEXIST;                  /* the root always exists */
    if (fs_lookup(norm)) return -AIOS_EEXIST;
    const char *bn; size_t bl;
    fsnode_t *d = fs_lookup_parent(norm, &bn, &bl);
    if (!d) return -AIOS_ENOENT;
    if (bl == 0 || bl >= FS_NAME_MAX) return -AIOS_ENAMETOOLONG;
    char nm[FS_NAME_MAX]; memcpy(nm, bn, bl); nm[bl] = '\0';
    fsnode_t *n = node_new(nm, 1, mode & 07777);
    if (!n) return -AIOS_ENOMEM;
    dir_link(d, n);
    return 0;
}

/* Is any open handle still pointing at this node? A detached-but-open node must NOT be freed --
 * handles hold raw node pointers, so freeing one is a use-after-free on the next read/write/fstat.
 * (The node then leaks until reboot: there is no refcount. Deliberate, and bounded in practice.) */
static int node_is_open(const fsnode_t *n) {
    for (int i = 0; i < TARFS_MAX_OPEN; i++)
        if (g_of[i].used && g_of[i].n == n) return 1;
    return 0;
}

/* Remove a normalized path. want_dir: rmdir semantics (must be an empty dir) vs unlink (must not
 * be a dir). An open handle keeps its node pointer, so a file unlinked while open stays readable
 * to that handle -- POSIX-ish (the node is detached from the tree but not freed). */
static int fs_remove_norm(const char *norm, int want_dir) {
    if (norm[0] == '\0') return -AIOS_EBUSY;                   /* never remove the root */
    fsnode_t *n = fs_lookup(norm);
    if (!n) return -AIOS_ENOENT;
    if (want_dir) {
        if (!n->is_dir) return -AIOS_ENOTDIR;
        if (n->child)  return -AIOS_ENOTEMPTY;
    } else {
        if (n->is_dir) return -AIOS_EISDIR;
    }
    fsnode_t *d = n->parent;
    dir_unlink(d, n);
    if (!node_is_open(n)) node_free(n);                        /* still open -> detach only */
    return 0;
}

int pal_host_mkdir(const char *path, unsigned int mode) {
    if (!g_root) return -AIOS_ENOSYS;
    char norm[FS_PATH_MAX];
    if (fs_norm(path, norm, sizeof norm) < 0) return -AIOS_ENAMETOOLONG;
    return fs_mkdir_norm(norm, mode);
}

int pal_host_rmdir(const char *path) {
    if (!g_root) return -AIOS_ENOSYS;
    char norm[FS_PATH_MAX];
    if (fs_norm(path, norm, sizeof norm) < 0) return -AIOS_ENAMETOOLONG;
    return fs_remove_norm(norm, 1);
}

int pal_host_unlink(const char *path) {
    if (!g_root) return -AIOS_ENOSYS;
    char norm[FS_PATH_MAX];
    if (fs_norm(path, norm, sizeof norm) < 0) return -AIOS_ENAMETOOLONG;
    return fs_remove_norm(norm, 0);
}

int pal_host_unlinkat(pal_file_t dir, const char *path, int rmdir_flag) {
    if (!g_root) return -AIOS_ENOSYS;
    char norm[FS_PATH_MAX];
    long e = at_resolve(dir, path, norm, sizeof norm);
    if (e) return (int)e;
    return fs_remove_norm(norm, rmdir_flag ? 1 : 0);
}

int pal_host_rename(const char *oldp, const char *newp) {
    if (!g_root) return -AIOS_ENOSYS;
    char on[FS_PATH_MAX], nn[FS_PATH_MAX];
    if (fs_norm(oldp, on, sizeof on) < 0 || fs_norm(newp, nn, sizeof nn) < 0) return -AIOS_ENAMETOOLONG;
    if (on[0] == '\0' || nn[0] == '\0') return -AIOS_EBUSY;
    fsnode_t *src = fs_lookup(on);
    if (!src) return -AIOS_ENOENT;
    const char *bn; size_t bl;
    fsnode_t *nd = fs_lookup_parent(nn, &bn, &bl);
    if (!nd || bl == 0 || bl >= FS_NAME_MAX) return -AIOS_ENOENT;
    for (fsnode_t *a = nd; a; a = a->parent) if (a == src) return -AIOS_EINVAL;  /* into its own subtree */
    fsnode_t *dst = fs_lookup(nn);
    if (dst) {                                                  /* replace the destination */
        if (dst == src) return 0;
        if (src->is_dir && !dst->is_dir) return -AIOS_ENOTDIR;  /* POSIX: dir onto a non-dir */
        if (!src->is_dir && dst->is_dir) return -AIOS_EISDIR;   /* POSIX: non-dir onto a dir */
        if (dst->is_dir && dst->child) return -AIOS_ENOTEMPTY;
        dir_unlink(dst->parent, dst);
        if (!node_is_open(dst)) node_free(dst);                 /* an open handle keeps it alive */
    }
    dir_unlink(src->parent, src);
    memcpy(src->name, bn, bl); src->name[bl] = '\0';
    dir_link(nd, src);
    return 0;
}

int pal_host_fchmodat(pal_file_t dir, const char *path, unsigned int mode, int nofollow) {
    (void)nofollow;
    if (!g_root) return -AIOS_ENOSYS;
    char norm[FS_PATH_MAX];
    long e = at_resolve(dir, path, norm, sizeof norm);
    if (e) return (int)e;
    fsnode_t *n = fs_lookup(norm);
    if (!n) return -AIOS_ENOENT;
    n->mode = mode & 07777;
    return 0;
}

int pal_host_utimensat(pal_file_t dir, const char *path, const struct aios_timespec *times, int nofollow) {
    (void)nofollow;
    if (!g_root) return -AIOS_ENOSYS;
    char norm[FS_PATH_MAX];
    long e = at_resolve(dir, path, norm, sizeof norm);
    if (e) return (int)e;
    fsnode_t *n = fs_lookup(norm);
    if (!n) return -AIOS_ENOENT;
    if (times) n->mtime = (long)times[1].tv_sec;   /* [0]=atime, [1]=mtime; we track only mtime */
    return 0;
}

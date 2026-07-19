/*
 * uk/pal/sel4/tarfs.h -- the AIOS-on-seL4 filesystem: a WRITABLE in-memory tree seeded from the
 * embedded aiosroot.tar (D.3; read-only in C.4). tarfs.c DEFINES the fs half of the PAL contract
 * (pal_host_{open,read,lseek,close,fstat,stat,getdents,*at,mkdir,rmdir,unlink,rename,...}); this
 * header is only the seam boot.c needs: mounting, resolving exec paths to bytes, and the file half
 * of pal_host_write.
 */
#ifndef AIOS_PAL_SEL4_TARFS_H
#define AIOS_PAL_SEL4_TARFS_H

#include "pal.h"                 /* pal_file_t */

/* Mount the archive at [base, base+size): parse it ONCE into the node tree. File bytes are NOT
 * copied -- a node points at the immortal tar image until the first write copies it up.
 * Returns the number of regular files seeded (0 = empty/unparsable; the fs ops then -ENOSYS). */
int tarfs_init(const void *base, unsigned long size);

/* Resolve an ABSOLUTE path to a regular file's bytes (the exec loader's seam). Normalizes dot
 * segments and strips the archive's top-level prefix directory. NULL = no such file. */
const void *tarfs_find(const char *abspath, unsigned long *size_out);

/* The fs half of pal_host_write (D.3, the writable RAM fs). boot.c owns the pal_host_write symbol
 * -- console 0/1/2 and pipe-end handles route there first -- and calls this for file handles.
 * Returns bytes written, or a negative AIOS errno. */
long tarfs_write(pal_file_t f, const void *buf, size_t n);

#endif

/*
 * uk/pal/sel4/tarfs.h -- the Phase C.4 read-only tarfs over the embedded aiosroot.tar.
 * tarfs.c also DEFINES the fs half of the PAL contract (pal_host_{open,read,lseek,close,fstat,
 * stat}); this header is only the seam boot.c needs: mounting, and resolving exec paths to bytes.
 */
#ifndef AIOS_PAL_SEL4_TARFS_H
#define AIOS_PAL_SEL4_TARFS_H

/* Mount the archive at [base, base+size) (immortal image memory -- nothing is copied).
 * Returns the number of regular files found (0 = empty/unparsable; the fs ops then -ENOSYS). */
int tarfs_init(const void *base, unsigned long size);

/* Resolve an ABSOLUTE path to a regular file's bytes (the exec loader's seam). Normalizes dot
 * segments and strips the archive's top-level prefix directory. NULL = no such file. */
const void *tarfs_find(const char *abspath, unsigned long *size_out);

#endif

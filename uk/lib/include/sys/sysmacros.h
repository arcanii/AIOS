/* sys/sysmacros.h -- AIOS shadow header. Device-number split, used by ls -l for device files.
 * A single flat device-number space for now (the host fills st_rdev). */
#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#define major(dev)      ((unsigned)(((dev) >> 8) & 0xfff))
#define minor(dev)      ((unsigned)((dev) & 0xff))
#define makedev(ma, mi) (((unsigned long long)(ma) << 8) | ((mi) & 0xff))

#endif /* _SYS_SYSMACROS_H */

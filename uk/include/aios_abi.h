/*
 * aios_abi.h -- the AIOS ABI. THIS is what AIOS programs see.
 *
 * Host-agnostic by construction: identical whether the AIOS kernel runs on Linux (syscalls
 * trapped via ptrace) or on a future verified seL4 (syscalls delivered via IPC). An AIOS
 * program invokes the platform syscall instruction with an AIOS syscall number in the syscall-
 * number register; AIOS -- not Linux -- owns these numbers and their semantics.
 *
 * The numbers are deliberately disjoint from any host kernel's so that "this is the AIOS ABI,
 * not the host ABI" is explicit, and any accidental host-ABI leakage is caught (an unknown
 * AIOS syscall returns -1). Grow this surface as the kernel grows (M1 = WRITE + EXIT).
 */
#ifndef AIOS_ABI_H
#define AIOS_ABI_H

#define AIOS_SYS_WRITE   0x1000   /* (fd, buf, len)         -> bytes written        */
#define AIOS_SYS_EXIT    0x1001   /* (code)                -> does not return       */
#define AIOS_SYS_OPEN    0x1002   /* (path, flags, mode)   -> aios fd, or -1        */
#define AIOS_SYS_READ    0x1003   /* (fd, buf, len)        -> bytes read (0 = EOF)  */
#define AIOS_SYS_CLOSE   0x1004   /* (fd)                  -> 0, or -1              */

/* AIOS open flags -- AIOS owns these values; the host PAL translates them to its native flags
 * (Linux O_*). The low 2 bits are the access mode. */
#define AIOS_O_RDONLY    0x0000
#define AIOS_O_WRONLY    0x0001
#define AIOS_O_RDWR      0x0002
#define AIOS_O_ACCMODE   0x0003
#define AIOS_O_CREAT     0x0100
#define AIOS_O_TRUNC     0x0200
#define AIOS_O_APPEND    0x0400

/* Reserved AIOS file descriptors (the kernel seeds these from the PAL std streams). */
#define AIOS_FD_STDIN    0
#define AIOS_FD_STDOUT   1
#define AIOS_FD_STDERR   2

#endif /* AIOS_ABI_H */

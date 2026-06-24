/*
 * pal.h -- the Platform Abstraction Layer: the ONLY surface the AIOS kernel uses to reach the
 * host. Narrow by design, because this seam is the future *verified boundary* -- the smaller it
 * is, the smaller the eventual proof obligation when AIOS replants onto seL4.
 *
 * The AIOS kernel (kernel/aios_kernel.c) includes ONLY this header and aios_abi.h -- never a host
 * header -- so the kernel is host-agnostic. Today the backend is pal/pal_linux.c (ptrace); a
 * future pal_sel4.c implements the SAME contract via seL4 IPC / a VMM. Swapping the backend does
 * not touch the kernel or any AIOS program.
 *
 * The trap model is gVisor-style: a guest program's syscall is *trapped* (not cooperatively
 * forwarded) and serviced here. pal_guest_trap_next() is the per-host trap primitive.
 */
#ifndef AIOS_PAL_H
#define AIOS_PAL_H

#include <stddef.h>
#include <stdint.h>

/* One intercepted guest syscall: the AIOS-ABI request a program made (see aios_abi.h). */
typedef struct {
    uint64_t nr;        /* AIOS syscall number */
    uint64_t arg[6];    /* arguments                                                       */
} pal_syscall_t;

/* --- guest world lifecycle --- */

/* Load + start the AIOS program at `path` under the host trap mechanism. Returns 0 on success. */
int pal_spawn_guest(const char *path);

/* Block until the guest's next syscall, or its exit. Returns:
 *   1  a syscall trapped  -> *out filled (the syscall is NOT executed by the host kernel)
 *   0  the guest exited   -> *exit_code set
 *  -1  error                                                                                */
int pal_guest_trap_next(pal_syscall_t *out, int *exit_code);

/* Set the value the guest sees returned from the syscall just trapped, and resume it. */
int pal_guest_return(uint64_t retval);

/* --- guest memory --- */

/* Copy `len` bytes from the guest address space at `gaddr` into `dst`. Returns bytes copied
 * (0 on failure). The AIOS kernel uses this to bounce syscall buffers out of the guest. */
size_t pal_guest_read(uint64_t gaddr, void *dst, size_t len);

/* --- host-driver gateway --- */

/* Write `len` bytes of `buf` to host fd `fd`. This is the AIOS kernel's only route to real
 * hardware/host services -- on Linux a host write(2); on seL4 a driver IPC. Returns bytes
 * written, or <0 on error. (M1 needs only output; the gateway grows with the kernel.) */
long pal_host_write(int fd, const void *buf, size_t len);

#endif /* AIOS_PAL_H */

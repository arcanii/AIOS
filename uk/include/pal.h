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

/* Load + start the AIOS program at `path` under the host trap mechanism, with `argv` as the
 * program's argument vector (argv[0] = the program, NULL-terminated). Returns 0 on success. */
int pal_spawn_guest(const char *path, char *const argv[]);

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

/* Copy `len` bytes from `src` INTO the guest address space at `gaddr`. Returns bytes copied
 * (0 on failure). Used to return syscall results (e.g. read data) back into the guest. */
size_t pal_guest_write(uint64_t gaddr, const void *src, size_t len);

/* --- host-driver gateway --- */
/* A backing object the host provides for a file/stream. Opaque to the AIOS kernel, which keeps
 * these in its own fd table and never assumes their representation (the Linux PAL uses a host
 * fd; a future seL4 PAL might use a server cap). */
typedef int64_t pal_file_t;
#define PAL_FILE_INVALID ((pal_file_t)-1)

/* The host's standard streams as backing handles. which: 0=stdin 1=stdout 2=stderr. The AIOS
 * kernel seeds its reserved fds (AIOS_FD_*) from these. */
pal_file_t pal_host_std(int which);

/* Open a host-backed file. `aios_flags` are AIOS_O_* (aios_abi.h) -- the PAL translates them to
 * native host flags. Returns a backing handle, or PAL_FILE_INVALID. This is the AIOS kernel's
 * route to real storage (Linux: open(2); seL4: an fs-server IPC). */
pal_file_t pal_host_open(const char *path, uint64_t aios_flags, uint64_t mode);

/* Read/write/close a backing object. write is also the kernel's diagnostic + stdout path.
 * Return bytes transferred (<0 on error) / 0 on close-OK. */
long pal_host_read (pal_file_t f, void *buf, size_t len);
long pal_host_write(pal_file_t f, const void *buf, size_t len);
int  pal_host_close(pal_file_t f);

#endif /* AIOS_PAL_H */

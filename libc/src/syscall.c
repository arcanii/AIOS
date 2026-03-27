/*
 * AIOS libc syscall transport
 *
 * All POSIX calls go through this layer, which uses seL4 PPC
 * to communicate with the VFS server.
 *
 * For now (before VFS exists), we use the sandbox_io shared memory
 * and PPC channel 7 to the orchestrator.
 */
#include <stdint.h>
#include <microkit.h>
#include <sys/syscall.h>

#define CH_VFS 7  /* PPC channel to orchestrator/VFS */

/* Shared memory for passing paths and data */
extern uintptr_t sandbox_io;

#define SYSCALL_PATH   0x200  /* offset for path strings */
#define SYSCALL_DATA   0x400  /* offset for data buffers */
#define SYSCALL_DATA_MAX 3072

/* Low-level syscall: sends syscall number + up to 3 args via MRs */
long __aios_syscall(long num, long a0, long a1, long a2) {
    seL4_SetMR(0, num);
    seL4_SetMR(1, a0);
    seL4_SetMR(2, a1);
    seL4_SetMR(3, a2);
    microkit_msginfo reply = microkit_ppcall(CH_VFS, microkit_msginfo_new(0, 4));
    (void)reply;
    return (long)seL4_GetMR(0);
}

/* Copy a string to shared memory at a given offset, return length */
static int copy_path(const char *path, int offset) {
    volatile char *dst = (volatile char *)(sandbox_io + offset);
    int i = 0;
    while (path[i] && i < 255) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    return i;
}

/* Copy data to shared memory */
static void copy_to_shm(const void *src, int offset, int len) {
    const uint8_t *s = (const uint8_t *)src;
    volatile uint8_t *d = (volatile uint8_t *)(sandbox_io + offset);
    for (int i = 0; i < len; i++) d[i] = s[i];
}

/* Copy data from shared memory */
static void copy_from_shm(void *dst, int offset, int len) {
    uint8_t *d = (uint8_t *)dst;
    volatile uint8_t *s = (volatile uint8_t *)(sandbox_io + offset);
    for (int i = 0; i < len; i++) d[i] = s[i];
}

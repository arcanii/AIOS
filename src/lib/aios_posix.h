#ifndef AIOS_POSIX_H
#define AIOS_POSIX_H

#include <sel4/sel4.h>

/*
 * AIOS POSIX Shim
 *
 * Exec thread passes caps as argv:
 *   argv[0] = serial_ep
 *   argv[1] = fs_ep
 *   argv[2] = thread_ep
 *   argv[3] = CWD
 *   argv[4..] = real program args
 */

/* Serial IPC labels */
#define AIOS_SER_PUTC     1
#define AIOS_SER_GETC     2
#define AIOS_SER_PUTS     3
#define AIOS_SER_KEY_PUSH 4

/* Filesystem IPC labels */
#define AIOS_FS_LS       10
#define AIOS_FS_CAT      11
#define AIOS_FS_STAT     12
#define AIOS_FS_OPEN     13
#define AIOS_FS_READ     14
#define AIOS_FS_CLOSE    15

/* Exec IPC labels */
#define AIOS_EXEC_RUN    20
#define AIOS_EXEC_NICE   21

/* Thread IPC labels */
#define AIOS_THREAD_CREATE  30
#define AIOS_THREAD_JOIN    31

/* Initialize shim with endpoint caps */
void aios_init(seL4_CPtr serial_ep, seL4_CPtr fs_ep);
void aios_init_full(seL4_CPtr serial_ep, seL4_CPtr fs_ep, seL4_CPtr thread_ep);

int aios_getchar(void);
void aios_set_cwd(const char *path);

seL4_CPtr aios_get_serial_ep(void);
seL4_CPtr aios_get_fs_ep(void);
seL4_CPtr aios_get_auth_ep(void);

static inline long _aios_parse(const char *s) {
    if (!s) return 0;
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

#define AIOS_INIT(argc, argv) \
    aios_init( \
        (argc) > 0 ? (seL4_CPtr)_aios_parse((argv)[0]) : 0, \
        (argc) > 1 ? (seL4_CPtr)_aios_parse((argv)[1]) : 0  \
    )

#endif

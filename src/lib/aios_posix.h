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

/* Pipe IPC labels */
#define AIOS_PIPE_CREATE    60
#define AIOS_PIPE_WRITE     61
#define AIOS_PIPE_READ      62
#define AIOS_PIPE_CLOSE     63
#define AIOS_PIPE_CLOSE_WRITE 70
#define AIOS_PIPE_CLOSE_READ  73

/* Exec with pipe redirection */
#define AIOS_EXEC_RUN_PIPE  22
#define AIOS_EXEC_KILL      23

/* Initialize shim with endpoint caps */
void aios_init(seL4_CPtr serial_ep, seL4_CPtr fs_ep);
void aios_init_caps(seL4_CPtr serial, seL4_CPtr fs,
                    seL4_CPtr auth, seL4_CPtr pip);
void aios_init_full(seL4_CPtr serial_ep, seL4_CPtr fs_ep, seL4_CPtr thread_ep);

int aios_getchar(void);
void aios_set_cwd(const char *path);

seL4_CPtr aios_get_serial_ep(void);
seL4_CPtr aios_get_fs_ep(void);
seL4_CPtr aios_get_auth_ep(void);
seL4_CPtr aios_get_thread_ep(void);
int aios_nb_getchar(void);  /* non-blocking, returns -1 if no input */

void aios_set_pipe_redirect(int stdout_pipe, int stdin_pipe);
int aios_get_pipe_id(int fd);

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

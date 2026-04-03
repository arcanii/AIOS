#ifndef AIOS_POSIX_H
#define AIOS_POSIX_H

#include <sel4/sel4.h>

/*
 * AIOS POSIX Shim
 *
 * Call aios_init() at the start of main() to route
 * stdio through IPC to serial_server, and file I/O
 * through IPC to fs_server.
 *
 * After init, standard POSIX calls work:
 *   printf()     → IPC PUTS to serial_server
 *   read(0,...)  → IPC GETC to serial_server
 *   open()       → IPC to fs_server (planned)
 */

/* IPC protocol labels (must match serial_server + fs_thread) */
#define AIOS_SER_PUTC     1
#define AIOS_SER_GETC     2
#define AIOS_SER_PUTS     3
#define AIOS_SER_KEY_PUSH 4
#define AIOS_FS_LS       10
#define AIOS_FS_CAT      11
#define AIOS_FS_STAT     12

/* Initialize POSIX shim with endpoint caps.
 * serial_ep: endpoint to serial_server (for stdin/stdout/stderr)
 * fs_ep:     endpoint to fs_thread (for open/read/close), 0 if none
 */
void aios_init(seL4_CPtr serial_ep, seL4_CPtr fs_ep);

/* Get the serial endpoint (for manual IPC if needed) */
seL4_CPtr aios_get_serial_ep(void);

/* Get the fs endpoint */
seL4_CPtr aios_get_fs_ep(void);

/* Read one char from serial via IPC */
int aios_getchar(void);

#endif

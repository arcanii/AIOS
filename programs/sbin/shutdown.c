#include "aios.h"

/* shutdown - cleanly halt the system (root only)
 *
 * Usage: shutdown [-h] [-r] [-n]
 *   -h   halt (default)
 *   -r   reboot (not yet supported)
 *   -n   skip filesystem sync
 *
 * Requires root privileges (uid 0).
 */

static aios_syscalls_t *sys_ptr;

#define SHUTDOWN_HALT    0
#define SHUTDOWN_REBOOT  1
#define SHUTDOWN_NOSYNC  2

int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    sys_ptr = _sys;

    int flags = SHUTDOWN_HALT;
    const char *args_str = (sys_ptr->args);

    /* Parse arguments */
    if (args_str && args_str[0]) {
        if (args_str[0] == '-') {
            for (int i = 1; args_str[i]; i++) {
                if (args_str[i] == 'r') flags |= SHUTDOWN_REBOOT;
                if (args_str[i] == 'n') flags |= SHUTDOWN_NOSYNC;
                if (args_str[i] == 'h') flags = SHUTDOWN_HALT;
            }
        }
    }

    /* Check root */
    if ((sys_ptr->getuid)() != 0) {
        puts("shutdown: must be root\n");
        return 1;
    }

    puts("\nThe system is going down for ");
    if (flags & SHUTDOWN_REBOOT)
        puts("reboot");
    else
        puts("halt");
    puts(" NOW!\n\n");

    /* Sync filesystems unless -n */
    if (!(flags & SHUTDOWN_NOSYNC)) {
        puts("Syncing filesystems... ");
        (sys_ptr->sync)();
        puts("done.\n");
    }

    /* Send all processes SIGTERM (future) */
    puts("Sending all processes the TERM signal.\n");

    /* Halt */
    (sys_ptr->shutdown)(flags);

    /* Should not reach here */
    puts("shutdown: halt failed\n");
    return 1;
}

/*
 * reboot.c -- Restart AIOS (BCM2711 watchdog reset on RPi4)
 * Sends PIPE_SHUTDOWN IPC with the reboot flag set (MR0 = 1).
 * Equivalent to `shutdown -r`. Only root (uid 0) is permitted.
 */
#include <stdio.h>
#include <stdlib.h>
#include "aios_posix.h"

#define PIPE_SHUTDOWN 77

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    seL4_CPtr pipe = aios_get_pipe_ep();
    if (!pipe) {
        fprintf(stderr, "reboot: no pipe endpoint\n");
        return 1;
    }

    printf("Requesting system reboot...\n");
    seL4_SetMR(0, (seL4_Word)1);   /* reboot flag */
    seL4_Call(pipe, seL4_MessageInfo_new(PIPE_SHUTDOWN, 0, 0, 1));

    long result = (long)seL4_GetMR(0);
    if (result != 0) {
        fprintf(stderr, "reboot: permission denied\n");
        return 1;
    }

    /* Should not reach here -- system resets */
    return 0;
}

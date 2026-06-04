/*
 * aios_shutdown.c -- Cleanly power off or reboot AIOS
 * Sends PIPE_SHUTDOWN IPC to pipe_server (MR0 = reboot flag).
 * Only root (uid 0) is permitted.
 *
 * Usage:  shutdown            halt the system
 *         shutdown -r         reboot (BCM2711 watchdog reset on RPi4)
 *         reboot              same as shutdown -r (argv[0] basename)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aios_posix.h"

#define PIPE_SHUTDOWN 77

int main(int argc, char *argv[]) {
    int do_reboot = 0;

    /* Reboot if invoked as `reboot`, or with -r / --reboot. */
    const char *base = argv[0] ? argv[0] : "";
    for (const char *p = base; *p; p++)
        if (*p == '/') base = p + 1;
    if (strcmp(base, "reboot") == 0)
        do_reboot = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--reboot") == 0)
            do_reboot = 1;
    }

    seL4_CPtr pipe = aios_get_pipe_ep();
    if (!pipe) {
        fprintf(stderr, "shutdown: no pipe endpoint\n");
        return 1;
    }

    printf("Requesting system %s...\n", do_reboot ? "reboot" : "shutdown");
    seL4_SetMR(0, (seL4_Word)do_reboot);
    seL4_Call(pipe, seL4_MessageInfo_new(PIPE_SHUTDOWN, 0, 0, 1));

    long result = (long)seL4_GetMR(0);
    if (result != 0) {
        fprintf(stderr, "shutdown: permission denied\n");
        return 1;
    }

    /* Should not reach here -- system halts or resets */
    return 0;
}

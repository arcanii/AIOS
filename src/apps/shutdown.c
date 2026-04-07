/*
 * aios_shutdown.c -- Cleanly power off AIOS
 * Sends PIPE_SHUTDOWN IPC to pipe_server.
 * Only root (uid 0) is permitted.
 */
#include <stdio.h>
#include <stdlib.h>
#include "aios_posix.h"

#define PIPE_SHUTDOWN 77

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    seL4_CPtr pipe = aios_get_pipe_ep();
    if (!pipe) {
        fprintf(stderr, "shutdown: no pipe endpoint\n");
        return 1;
    }

    printf("Requesting system shutdown...\n");
    seL4_Call(pipe, seL4_MessageInfo_new(PIPE_SHUTDOWN, 0, 0, 0));

    long result = (long)seL4_GetMR(0);
    if (result != 0) {
        fprintf(stderr, "shutdown: permission denied\n");
        return 1;
    }

    /* Should not reach here -- system halts */
    return 0;
}

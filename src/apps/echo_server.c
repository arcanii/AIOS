/*
 * AIOS 0.4.x — echo server
 * Endpoint cap slot is passed as argv[0].
 */
#include <stdio.h>
#include <stdlib.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[]) {
    /* Default EP slot, or from argv */
    seL4_CPtr ep = 8;
    if (argc > 0 && argv[0]) {
        ep = (seL4_CPtr)atol(argv[0]);
    }
    printf("[echo_server] Started on EP slot %lu\n", (unsigned long)ep);

    for (int i = 0; i < 5; i++) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word val = seL4_GetMR(0);
        printf("[echo_server] msg %d: val=%lu\n", i, (unsigned long)val);

        seL4_SetMR(0, val + 1);
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
    }

    printf("[echo_server] Done. Exiting.\n");
    return 0;
}

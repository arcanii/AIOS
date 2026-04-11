/* sshd_main.c -- AIOS SSH server main loop
 *
 * Phase 1: TCP listen, version exchange, KEXINIT negotiation.
 * Single-connection sequential server (like echo_tcp).
 *
 * Usage: sshd            (listens on port 2222)
 * Test:  ssh -v -p 2222 -o StrictHostKeyChecking=no root@localhost
 */

#include "ssh_session.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("[sshd] AIOS SSH server starting\n");

    /* Initialize crypto (DRBG + host key) */
    if (ssh_crypto_init() < 0) {
        printf("[sshd] Crypto init failed\n");
        return 1;
    }

    /* Create listening socket */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        printf("[sshd] socket() failed\n");
        return 1;
    }

    /* Bind to port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SSHD_PORT);
    addr.sin_addr.s_addr = 0;

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[sshd] bind(%d) failed\n", SSHD_PORT);
        close(lfd);
        return 1;
    }

    if (listen(lfd, 1) < 0) {
        printf("[sshd] listen() failed\n");
        close(lfd);
        return 1;
    }

    printf("[sshd] Listening on port %d\n", SSHD_PORT);

    /* Accept loop (one connection at a time) */
    while (1) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            printf("[sshd] accept() failed\n");
            continue;
        }

        printf("[sshd] Client connected (fd %d)\n", cfd);

        /* Initialize session state */
        ssh_session_t sess;
        memset(&sess, 0, sizeof(sess));
        sess.sockfd = cfd;

        /* Phase 1: Version exchange */
        if (ssh_version_exchange(&sess) < 0) {
            printf("[sshd] Version exchange failed\n");
            close(cfd);
            continue;
        }

        /* Phase 1: KEXINIT exchange + algorithm negotiation */
        if (ssh_do_kexinit(&sess) < 0) {
            printf("[sshd] KEXINIT failed\n");
            ssh_disconnect(&sess, SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
                          "algorithm negotiation failed");
            close(cfd);
            continue;
        }

        /* Phase 2: ECDH key exchange + NEWKEYS */
        if (ssh_do_kex_exchange(&sess) < 0) {
            printf("[sshd] Key exchange failed\n");
            ssh_disconnect(&sess, SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
                          "key exchange failed");
            close(cfd);
            continue;
        }

        /* Phase 3 will continue here with encrypted transport:
         *   SERVICE_REQUEST, user auth, channel open, shell
         * For now, note that encryption is active but not implemented.
         * The next client packet will be encrypted -- we cannot read it. */
        printf("[sshd] Phase 2 complete. Encryption not yet implemented.\n");

        ssh_disconnect(&sess, SSH_DISCONNECT_BY_APPLICATION,
                      "key exchange complete, encryption pending");
        close(cfd);
        printf("[sshd] Connection closed, waiting for next\n\n");
    }

    close(lfd);
    return 0;
}

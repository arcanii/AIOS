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
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* Pre-auth grace period (v0.4.187): a connection must reach
 * USERAUTH_SUCCESS within this window or it is dropped, so an idle or
 * dribbling client cannot hold the single-session server forever.
 * 60s covers a hand-typed password plus the full failed-attempt
 * backoff (2+5+8 = 15s); OpenSSH ships LoginGraceTime 120s. */
#define SSHD_PREAUTH_GRACE_SECS 60

int g_ssh_verbose = 0;

int main(int argc, char **argv)
{
    int ai;
    for (ai = 1; ai < argc; ai++)
        if (argv[ai][0] == '-' && argv[ai][1] == 'v') g_ssh_verbose = 1;

    signal(SIGINT, SIG_IGN);  /* v0.4.85: ignore local Ctrl-C */
    SSHLOG("[sshd] AIOS SSH server starting\n");

    /* Initialize crypto (DRBG + host key) */
    if (ssh_crypto_init() < 0) {
        SSHLOG("[sshd] Crypto init failed\n");
        return 1;
    }

    /* Create listening socket */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        SSHLOG("[sshd] socket() failed\n");
        return 1;
    }

    /* Bind to port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SSHD_PORT);
    addr.sin_addr.s_addr = 0;

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        SSHLOG("[sshd] bind(%d) failed\n", SSHD_PORT);
        close(lfd);
        return 1;
    }

    if (listen(lfd, 1) < 0) {
        SSHLOG("[sshd] listen() failed\n");
        close(lfd);
        return 1;
    }

    SSHLOG("[sshd] Listening on port %d\n", SSHD_PORT);

    /* Accept loop (one connection at a time) */
    while (1) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            SSHLOG("[sshd] accept() failed\n");
            continue;
        }

        SSHLOG("[sshd] Client connected (fd %d)\n", cfd);

        /* Initialize session state */
        ssh_session_t sess;
        memset(&sess, 0, sizeof(sess));
        sess.sockfd = cfd;

        /* Arm the pre-auth deadline; O_NONBLOCK lets reads poll
         * against it instead of blocking in net_server (v0.4.187). */
        {
            int fl = fcntl(cfd, F_GETFL, 0);
            fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
        }
        ssh_set_preauth_deadline(SSHD_PREAUTH_GRACE_SECS);

        /* Phase 1: Version exchange */
        if (ssh_version_exchange(&sess) < 0) {
            SSHLOG("[sshd] Version exchange failed\n");
            close(cfd);
            continue;
        }

        /* Phase 1: KEXINIT exchange + algorithm negotiation */
        if (ssh_do_kexinit(&sess) < 0) {
            SSHLOG("[sshd] KEXINIT failed\n");
            ssh_disconnect(&sess, SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
                          "algorithm negotiation failed");
            close(cfd);
            continue;
        }

        /* Phase 2: ECDH key exchange + NEWKEYS */
        if (ssh_do_kex_exchange(&sess) < 0) {
            SSHLOG("[sshd] Key exchange failed\n");
            ssh_disconnect(&sess, SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
                          "key exchange failed");
            close(cfd);
            continue;
        }

        /* Phase 3: Initialize encrypted transport */
        if (ssh_encrypt_init(&sess) < 0) {
            SSHLOG("[sshd] Encryption init failed\n");
            close(cfd);
            continue;
        }

        SSHLOG("[sshd] === Encrypted transport active ===\n");

        /* Read packets until SERVICE_REQUEST */
        {
            int got_service = 0;
            int srounds = 0;
            uint8_t spkt[SSH_BUF_SIZE];
            int splen = 0;

            while (!got_service) {
                /* IGNORE/DEBUG/EXT_INFO/unexpected packets loop here
                 * without progress -- cap the rounds (v0.4.187). */
                if (++srounds > 16) {
                    SSHLOG("[sshd] Too many pre-service packets\n");
                    break;
                }
                if (ssh_read_packet(&sess, spkt, &splen) < 0) {
                    SSHLOG("[sshd] Encrypted read failed\n");
                    break;
                }
                if (splen < 1) break;

                uint8_t mtype = spkt[0];
                SSHLOG("[sshd] Encrypted msg type %d (%d bytes)\n",
                       mtype, splen);

                if (mtype == SSH_MSG_EXT_INFO) {
                    SSHLOG("[sshd] EXT_INFO received (skipping)\n");
                } else if (mtype == SSH_MSG_IGNORE ||
                           mtype == SSH_MSG_DEBUG) {
                    /* skip */
                } else if (mtype == SSH_MSG_DISCONNECT) {
                    SSHLOG("[sshd] Client disconnected\n");
                    break;
                } else if (mtype == SSH_MSG_SERVICE_REQUEST) {
                    int soff = 1;
                    const uint8_t *svc;
                    uint32_t svc_len;
                    if (ssh_get_string(spkt, splen, &soff,
                                       &svc, &svc_len) < 0) {
                        SSHLOG("[sshd] Bad SERVICE_REQUEST\n");
                        break;
                    }

                    SSHLOG("[sshd] SERVICE_REQUEST: %.*s\n",
                           (int)svc_len, (const char *)svc);

                    if (svc_len == 12 &&
                        memcmp(svc, "ssh-userauth", 12) == 0) {
                        /* Send SERVICE_ACCEPT */
                        uint8_t resp[20];
                        int roff = 0;
                        resp[roff++] = SSH_MSG_SERVICE_ACCEPT;
                        ssh_put_namelist(resp, "ssh-userauth", &roff);
                        if (ssh_write_packet(&sess, resp, roff) < 0) {
                            SSHLOG("[sshd] SERVICE_ACCEPT failed\n");
                            break;
                        }
                        SSHLOG("[sshd] SERVICE_ACCEPT sent\n");
                        got_service = 1;
                    } else {
                        SSHLOG("[sshd] Unknown service\n");
                        break;
                    }
                } else {
                    SSHLOG("[sshd] Unexpected msg %d\n", mtype);
                }
            }

            if (!got_service) {
                close(cfd);
                SSHLOG("[sshd] Service negotiation failed\n\n");
                continue;
            }
        }

        /* Phase 4: User authentication */
        SSHLOG("[sshd] Phase 3 complete -- starting authentication\n");

        if (ssh_do_userauth(&sess) < 0) {
            SSHLOG("[sshd] Authentication failed\n");
            ssh_disconnect(&sess, SSH_DISCONNECT_BY_APPLICATION,
                          "authentication failed");
            close(cfd);
            SSHLOG("[sshd] Connection closed, waiting for next\n\n");
            continue;
        }

        SSHLOG("[sshd] User authenticated (uid=%u)\n",
               (unsigned)sess.uid);

        /* Disarm the pre-auth deadline and restore blocking reads;
         * the channel relay manages its own O_NONBLOCK (v0.4.187). */
        ssh_set_preauth_deadline(0);
        {
            int fl = fcntl(cfd, F_GETFL, 0);
            fcntl(cfd, F_SETFL, fl & ~O_NONBLOCK);
        }

        /* Phase 5: Session channel + shell */
        SSHLOG("[sshd] Phase 4 complete -- starting channel\n");

        if (ssh_do_channel(&sess) < 0) {
            SSHLOG("[sshd] Channel session failed\n");
        } else {
            SSHLOG("[sshd] Channel session ended normally\n");
        }

        ssh_disconnect(&sess, SSH_DISCONNECT_BY_APPLICATION,
                      "session ended");
        close(cfd);
        /* v0.4.178: release the auth_server session slot so sequential
         * reconnects do not exhaust the (4-slot) session table. */
        ssh_auth_logout(sess.auth_token);
        SSHLOG("[sshd] Connection closed, waiting for next\n\n");
    }

    close(lfd);
    return 0;
}

/* ssh_channel.c -- SSH session channel + shell relay (RFC 4254)
 *
 * Phase 5: Opens session channel, handles pty-req and shell requests,
 * spawns /bin/dash via fork+exec, relays data between SSH channel
 * and shell stdin/stdout pipes.
 *
 * Data relay uses O_NONBLOCK polling on both socket and pipe with
 * nanosleep(10ms) between iterations to avoid busy-spin.
 */

#include "ssh_session.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>

/* Shell to spawn */
#define SSH_SHELL_PATH  "/bin/dash"
#define SSH_SHELL_NAME  "dash"

/* Channel window sizes */
#define SSH_CHAN_WINDOW   (64 * 1024)   /* 64KB initial window */
#define SSH_CHAN_MAX_PKT  (32 * 1024)   /* 32KB max packet */

/* Relay poll interval */
#define RELAY_POLL_NS    (10 * 1000 * 1000)  /* 10ms */

/* Line buffer for server-side echo */
#define LINE_BUF_SIZE    256

/* ----------------------------------------------------------------
 * Channel message senders
 * ---------------------------------------------------------------- */

static int send_chan_open_confirm(ssh_session_t *s)
{
    uint8_t p[32];
    int off = 0;
    p[off++] = SSH_MSG_CHANNEL_OPEN_CONFIRM;
    ssh_put_u32(p + off, s->client_channel);  off += 4;
    ssh_put_u32(p + off, s->server_channel);  off += 4;
    ssh_put_u32(p + off, SSH_CHAN_WINDOW);     off += 4;
    ssh_put_u32(p + off, SSH_CHAN_MAX_PKT);    off += 4;
    return ssh_write_packet(s, p, off);
}

static int send_chan_success(ssh_session_t *s)
{
    uint8_t p[8];
    int off = 0;
    p[off++] = SSH_MSG_CHANNEL_SUCCESS;
    ssh_put_u32(p + off, s->client_channel);  off += 4;
    return ssh_write_packet(s, p, off);
}

static int send_chan_failure(ssh_session_t *s)
{
    uint8_t p[8];
    int off = 0;
    p[off++] = SSH_MSG_CHANNEL_FAILURE;
    ssh_put_u32(p + off, s->client_channel);  off += 4;
    return ssh_write_packet(s, p, off);
}

static int send_chan_data(ssh_session_t *s,
                          const uint8_t *data, int len)
{
    if (len <= 0) return 0;
    uint8_t p[SSH_BUF_SIZE];
    int off = 0;
    p[off++] = SSH_MSG_CHANNEL_DATA;
    ssh_put_u32(p + off, s->client_channel);  off += 4;
    ssh_put_string(p, data, (uint32_t)len, &off);
    return ssh_write_packet(s, p, off);
}

static int send_window_adjust(ssh_session_t *s, uint32_t bytes)
{
    uint8_t p[16];
    int off = 0;
    p[off++] = SSH_MSG_CHANNEL_WINDOW_ADJUST;
    ssh_put_u32(p + off, s->client_channel);  off += 4;
    ssh_put_u32(p + off, bytes);              off += 4;
    return ssh_write_packet(s, p, off);
}

static int send_chan_eof(ssh_session_t *s)
{
    uint8_t p[8];
    int off = 0;
    p[off++] = SSH_MSG_CHANNEL_EOF;
    ssh_put_u32(p + off, s->client_channel);  off += 4;
    return ssh_write_packet(s, p, off);
}

static int send_chan_close(ssh_session_t *s)
{
    uint8_t p[8];
    int off = 0;
    p[off++] = SSH_MSG_CHANNEL_CLOSE;
    ssh_put_u32(p + off, s->client_channel);  off += 4;
    return ssh_write_packet(s, p, off);
}

/* ----------------------------------------------------------------
 * Handle CHANNEL_OPEN "session" (RFC 4254 section 5.1)
 * ---------------------------------------------------------------- */

static int handle_channel_open(ssh_session_t *s,
                                const uint8_t *pkt, int plen)
{
    int off = 1;
    const uint8_t *ctype;
    uint32_t ctype_len;
    if (ssh_get_string(pkt, plen, &off, &ctype, &ctype_len) < 0)
        return -1;

    if (ctype_len != 7 || memcmp(ctype, "session", 7) != 0) {
        printf("[sshd] Unsupported channel type: %.*s\n",
               (int)ctype_len, (const char *)ctype);
        /* Send CHANNEL_OPEN_FAILURE */
        uint8_t r[48];
        int ro = 0;
        r[ro++] = SSH_MSG_CHANNEL_OPEN_FAILURE;
        if (off + 4 <= plen)
            ssh_put_u32(r + ro, ssh_get_u32(pkt + off));
        else
            ssh_put_u32(r + ro, 0);
        ro += 4;
        ssh_put_u32(r + ro, 1);  ro += 4;  /* administratively prohibited */
        ssh_put_string(r, "only session channels", 21, &ro);
        ssh_put_u32(r + ro, 0);  ro += 4;
        return ssh_write_packet(s, r, ro);
    }

    if (off + 12 > plen) return -1;
    s->client_channel = ssh_get_u32(pkt + off);  off += 4;
    s->client_window  = ssh_get_u32(pkt + off);  off += 4;
    s->client_max_pkt = ssh_get_u32(pkt + off);  off += 4;
    s->server_channel = 0;
    s->server_window  = SSH_CHAN_WINDOW;
    s->channel_open   = 1;

    printf("[sshd] CHANNEL_OPEN session (ch=%u win=%u maxpkt=%u)\n",
           (unsigned)s->client_channel,
           (unsigned)s->client_window,
           (unsigned)s->client_max_pkt);

    return send_chan_open_confirm(s);
}

/* ----------------------------------------------------------------
 * Handle CHANNEL_REQUEST "pty-req" (RFC 4254 section 6.2)
 * ---------------------------------------------------------------- */

static int handle_pty_req(ssh_session_t *s,
                           const uint8_t *pkt, int plen,
                           int off, int want_reply)
{
    const uint8_t *term_str;
    uint32_t term_len;
    if (ssh_get_string(pkt, plen, &off, &term_str, &term_len) < 0)
        return -1;
    if (off + 16 > plen) return -1;

    s->term_width  = ssh_get_u32(pkt + off);  off += 4;
    s->term_height = ssh_get_u32(pkt + off);  off += 4;
    off += 8;  /* skip pixel dimensions */

    /* Skip terminal modes string */
    const uint8_t *modes;
    uint32_t modes_len;
    ssh_get_string(pkt, plen, &off, &modes, &modes_len);

    printf("[sshd] pty-req: %.*s %ux%u\n",
           (int)(term_len > 31 ? 31 : term_len),
           (const char *)term_str,
           (unsigned)s->term_width,
           (unsigned)s->term_height);

    s->has_pty = 1;
    if (want_reply) return send_chan_success(s);
    return 0;
}

/* ----------------------------------------------------------------
 * Spawn shell via fork + dup2 + execv
 *
 * Creates two pipes:
 *   stdin_pipe:  parent writes -> child reads  (child fd 0)
 *   stdout_pipe: child writes -> parent reads  (child fd 1+2)
 *
 * Returns 0 on success, -1 on failure.
 * ---------------------------------------------------------------- */

static int spawn_shell(int *stdin_wr, int *stdout_rd,
                       int *stdout_wr_fd, pid_t *child_pid)
{
    int in_pipe[2], out_pipe[2];

    if (pipe2(in_pipe, 0) < 0) {
        printf("[sshd] stdin pipe2 failed\n");
        return -1;
    }
    if (pipe2(out_pipe, 0) < 0) {
        printf("[sshd] stdout pipe2 failed\n");
        close(in_pipe[0]);
        close(in_pipe[1]);
        return -1;
    }

    printf("[sshd] pipes: in=[%d,%d] out=[%d,%d]\n",
           in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]);

    pid_t pid = fork();
    if (pid < 0) {
        printf("[sshd] fork failed\n");
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: redirect stdin/stdout/stderr to pipes */
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        dup2(out_pipe[1], 2);
        /* Do NOT close pipe ends here. In AIOS, close() on a pipe
         * write end sends PIPE_CLOSE_WRITE to the server, which
         * decrements the writer count. Closing in_pipe[1] would set
         * writers=0 for stdin, causing the shell to get immediate EOF.
         * exec replaces the entire process via PIPE_EXEC anyway. */
        char *argv[] = { (char *)SSH_SHELL_NAME, "-i", (void *)0 };
        execv(SSH_SHELL_PATH, argv);
        _exit(127);
    }

    /* Parent: close read end of stdin pipe (safe: no server notification).
     * Keep out_pipe[1] open -- closing it sets writers=0 in pipe_server,
     * which causes immediate EOF before the child can produce output.
     * The write end is returned for cleanup after the relay. */
    close(in_pipe[0]);
    *stdin_wr    = in_pipe[1];
    *stdout_rd   = out_pipe[0];
    *stdout_wr_fd = out_pipe[1];
    *child_pid   = pid;

    printf("[sshd] shell spawned pid=%d stdin_wr=%d stdout_rd=%d\n",
           (int)pid, *stdin_wr, *stdout_rd);
    return 0;
}

/* ----------------------------------------------------------------
 * Server-side line discipline (echo + line buffering)
 *
 * SSH client sends raw keystrokes when PTY is requested.
 * We echo printable chars, handle backspace and Enter,
 * send complete lines to the shell.
 *
 * Returns 0 normally, -1 if stdin should be closed (Ctrl-D).
 * ---------------------------------------------------------------- */

static int process_input(ssh_session_t *s, int stdin_wr,
                          const uint8_t *data, int dlen,
                          uint8_t *lbuf, int *lpos, pid_t child)
{
    int i;
    for (i = 0; i < dlen; i++) {
        uint8_t ch = data[i];

        if (ch == '\r' || ch == '\n') {
            /* Enter: echo CR+LF, send buffered line + LF to shell */
            uint8_t crlf[2] = { '\r', '\n' };
            send_chan_data(s, crlf, 2);
            if (*lpos > 0)
                write(stdin_wr, lbuf, *lpos);
            uint8_t lf = '\n';
            write(stdin_wr, &lf, 1);
            *lpos = 0;

        } else if (ch == 127 || ch == '\b') {
            /* Backspace: erase one character */
            if (*lpos > 0) {
                (*lpos)--;
                uint8_t erase[3] = { '\b', ' ', '\b' };
                send_chan_data(s, erase, 3);
            }

        } else if (ch == 3) {
            /* Ctrl-C: send SIGINT + write interrupt to stdin */
            kill(0, 2);  /* SIGINT to fg process via pipe_server */
            uint8_t intr = 3;
            write(stdin_wr, &intr, 1);
            uint8_t lf = '\n';
            write(stdin_wr, &lf, 1);
            uint8_t out[4] = { '^', 'C', '\r', '\n' };
            send_chan_data(s, out, 4);
            *lpos = 0;

        } else if (ch == 4) {
            /* Ctrl-D: send any buffered data, signal EOF */
            if (*lpos > 0) {
                write(stdin_wr, lbuf, *lpos);
                *lpos = 0;
            }
            return -1;

        } else if (ch >= 32 && ch < 127) {
            /* Printable: buffer + echo */
            if (*lpos < LINE_BUF_SIZE - 1) {
                lbuf[(*lpos)++] = ch;
                send_chan_data(s, &ch, 1);
            }
        }
        /* Other control chars: silently ignored */
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Bidirectional data relay
 *
 * Polls both the shell stdout pipe (O_NONBLOCK) and the SSH
 * socket (via ssh_read_packet_nb) in a loop. Sleeps 10ms when
 * neither source has data.
 * ---------------------------------------------------------------- */

static int channel_relay(ssh_session_t *s,
                          int stdin_wr, int stdout_rd,
                          int stdout_wr_fd, pid_t child)
{
    /* Set O_NONBLOCK on stdout pipe read end */
    int pfl = fcntl(stdout_rd, F_GETFL, 0);
    fcntl(stdout_rd, F_SETFL, pfl | 0x800);

    /* Set O_NONBLOCK on socket for truly non-blocking packet reads */
    int sfl = fcntl(s->sockfd, F_GETFL, 0);
    fcntl(s->sockfd, F_SETFL, sfl | 0x800);

    int done = 0;
    int ever_got_output = 0;
    uint8_t rbuf[900];
    uint8_t pkt[SSH_BUF_SIZE];
    int plen;

    /* Line buffer for server-side echo */
    uint8_t lbuf[LINE_BUF_SIZE];
    int lpos = 0;

    printf("[sshd] relay loop starting\n");

    while (!done) {
        int activity = 0;
        /* 1. Read shell output (non-blocking pipe read) */
        int n = (int)read(stdout_rd, rbuf, sizeof(rbuf));
        if (n > 0) {
            ever_got_output = 1;
            activity = 1;
            /* Convert LF to CRLF for SSH terminal display */
            uint8_t crbuf[1800];
            int ci = 0;
            int i;
            for (i = 0; i < n; i++) {
                if (rbuf[i] == '\n')
                    crbuf[ci++] = '\r';
                crbuf[ci++] = rbuf[i];
            }
            if (s->client_window >= (uint32_t)ci) {
                s->client_window -= (uint32_t)ci;
                if (send_chan_data(s, crbuf, ci) < 0) {
                    done = 1;
                    break;
                }
            }
        } else if (n == 0 && ever_got_output) {
            /* Real EOF: shell has exited (ignore spurious 0 before
             * any output -- caused by AIOS pipe SHM race) */
            printf("[sshd] shell exited (pipe EOF)\n");
            send_chan_eof(s);
            send_chan_close(s);
            done = 1;
        }

        /* 2. Read SSH packets (fully non-blocking) */
        if (!done) {
            plen = 0;
            int rc = ssh_read_packet_nb(s, pkt, &plen);
            if (rc == 0 && plen > 0) {
                activity = 1;
                uint8_t mtype = pkt[0];

                if (mtype == SSH_MSG_CHANNEL_DATA) {
                    int doff = 1 + 4;  /* skip type + recipient channel */
                    const uint8_t *data;
                    uint32_t data_len;
                    if (ssh_get_string(pkt, plen, &doff,
                                       &data, &data_len) == 0) {
                        if (s->has_pty) {
                            /* PTY mode: echo + line buffering */
                            if (process_input(s, stdin_wr, data,
                                              (int)data_len,
                                              lbuf, &lpos, child) < 0) {
                                /* Ctrl-D: close stdin */
                                close(stdin_wr);
                                stdin_wr = -1;
                            }
                        } else {
                            /* Raw mode: pass through directly */
                            int total = 0;
                            while (total < (int)data_len) {
                                int chunk = (int)data_len - total;
                                if (chunk > 900) chunk = 900;
                                int w = (int)write(stdin_wr,
                                                   data + total, chunk);
                                if (w <= 0) break;
                                total += w;
                            }
                        }
                        /* Replenish our receive window */
                        s->server_window -= data_len;
                        if (s->server_window < SSH_CHAN_WINDOW / 2) {
                            uint32_t adj = SSH_CHAN_WINDOW - s->server_window;
                            send_window_adjust(s, adj);
                            s->server_window += adj;
                        }
                    }

                } else if (mtype == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
                    int woff = 1 + 4;
                    if (woff + 4 <= plen)
                        s->client_window += ssh_get_u32(pkt + woff);

                } else if (mtype == SSH_MSG_CHANNEL_EOF) {
                    printf("[sshd] client CHANNEL_EOF\n");
                    if (stdin_wr >= 0) {
                        close(stdin_wr);
                        stdin_wr = -1;
                    }

                } else if (mtype == SSH_MSG_CHANNEL_CLOSE) {
                    printf("[sshd] client CHANNEL_CLOSE\n");
                    done = 1;

                } else if (mtype == SSH_MSG_DISCONNECT) {
                    printf("[sshd] client disconnected\n");
                    done = 1;

                } else if (mtype == SSH_MSG_IGNORE ||
                           mtype == SSH_MSG_DEBUG) {
                    /* skip */
                } else {
                    printf("[sshd] relay: unhandled msg %d\n", mtype);
                }

            } else if (rc < 0) {
                printf("[sshd] socket read error in relay\n");
                done = 1;
            }
        }

        /* 3. Idle detection: if shell produced output before but has
         * been silent, close the write end to trigger EOF */
        if (!activity && !done) {
            struct timespec ts = { 0, RELAY_POLL_NS };
            nanosleep(&ts, NULL);
        }
    }

    /* Cleanup: close write end first (triggers EOF for readers),
     * then close read end, then reap child. */
    if (stdin_wr >= 0) close(stdin_wr);
    if (stdout_wr_fd >= 0) close(stdout_wr_fd);
    close(stdout_rd);

    /* Reap child (should be done since pipe EOF) */
    int status = 0;
    waitpid(child, &status, 0);
    printf("[sshd] shell reaped (status=%d)\n", status);

    printf("[sshd] relay loop ended\n");
    return 0;
}

/* ----------------------------------------------------------------
 * Main channel handler -- called after successful auth
 *
 * 1. Wait for CHANNEL_OPEN "session"
 * 2. Handle CHANNEL_REQUEST (pty-req, env, shell)
 * 3. Spawn shell, enter data relay
 *
 * Returns 0 on clean exit, -1 on error.
 * ---------------------------------------------------------------- */

int ssh_do_channel(ssh_session_t *s)
{
    uint8_t pkt[SSH_BUF_SIZE];
    int plen;

    /* ---- Wait for CHANNEL_OPEN ---- */
    while (1) {
        if (ssh_read_packet(s, pkt, &plen) < 0) return -1;
        if (plen < 1) return -1;
        uint8_t mtype = pkt[0];

        if (mtype == SSH_MSG_IGNORE || mtype == SSH_MSG_DEBUG)
            continue;
        if (mtype == SSH_MSG_DISCONNECT) {
            printf("[sshd] disconnect before channel open\n");
            return -1;
        }
        if (mtype == SSH_MSG_CHANNEL_OPEN) {
            if (handle_channel_open(s, pkt, plen) < 0) return -1;
            break;
        }
        if (mtype == SSH_MSG_GLOBAL_REQUEST) {
            /* Reject global requests */
            uint8_t r[1];
            r[0] = SSH_MSG_REQUEST_FAILURE;
            ssh_write_packet(s, r, 1);
            continue;
        }
        printf("[sshd] expected CHANNEL_OPEN, got %d\n", mtype);
        return -1;
    }

    /* ---- Handle channel requests until "shell" ---- */
    int shell_requested = 0;
    while (!shell_requested) {
        if (ssh_read_packet(s, pkt, &plen) < 0) return -1;
        if (plen < 1) return -1;
        uint8_t mtype = pkt[0];

        if (mtype == SSH_MSG_IGNORE || mtype == SSH_MSG_DEBUG)
            continue;
        if (mtype == SSH_MSG_DISCONNECT) return -1;

        if (mtype == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
            int woff = 1 + 4;
            if (woff + 4 <= plen)
                s->client_window += ssh_get_u32(pkt + woff);
            continue;
        }

        if (mtype != SSH_MSG_CHANNEL_REQUEST) {
            printf("[sshd] expected CHANNEL_REQUEST, got %d\n", mtype);
            continue;
        }

        /* Parse CHANNEL_REQUEST */
        int off = 1 + 4;  /* skip type + recipient channel */
        const uint8_t *rtype;
        uint32_t rtype_len;
        if (ssh_get_string(pkt, plen, &off, &rtype, &rtype_len) < 0) {
            printf("[sshd] bad CHANNEL_REQUEST format\n");
            return -1;
        }
        int want_reply = 0;
        if (off < plen) want_reply = pkt[off++];

        printf("[sshd] CHANNEL_REQUEST: %.*s (reply=%d)\n",
               (int)rtype_len, (const char *)rtype, want_reply);

        if (rtype_len == 7 && memcmp(rtype, "pty-req", 7) == 0) {
            if (handle_pty_req(s, pkt, plen, off, want_reply) < 0)
                return -1;

        } else if (rtype_len == 3 && memcmp(rtype, "env", 3) == 0) {
            /* Accept but ignore env vars */
            if (want_reply) send_chan_success(s);

        } else if (rtype_len == 5 && memcmp(rtype, "shell", 5) == 0) {
            shell_requested = 1;
            if (want_reply) {
                if (send_chan_success(s) < 0) return -1;
            }

        } else if (rtype_len == 4 && memcmp(rtype, "exec", 4) == 0) {
            printf("[sshd] exec not yet supported\n");
            if (want_reply) send_chan_failure(s);

        } else {
            printf("[sshd] unknown request: %.*s\n",
                   (int)rtype_len, (const char *)rtype);
            if (want_reply) send_chan_failure(s);
        }
    }

    /* ---- Spawn shell and enter relay ---- */
    int stdin_wr = -1, stdout_rd = -1, stdout_wr_fd = -1;
    pid_t child_pid = -1;

    if (spawn_shell(&stdin_wr, &stdout_rd, &stdout_wr_fd, &child_pid) < 0) {
        printf("[sshd] shell spawn failed\n");
        send_chan_close(s);
        return -1;
    }

    int rc = channel_relay(s, stdin_wr, stdout_rd, stdout_wr_fd, child_pid);
    s->channel_open = 0;
    return rc;
}

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
/* v0.4.178: max DATA the CLIENT may put in one CHANNEL_DATA. The transport caps
 * an SSH payload at SSH_MAX_PAYLOAD (2048), and a CHANNEL_DATA payload is
 * data + 9 (type + recipient channel + length), so data must stay <= 2039.
 * 2000 leaves margin. The client then chunks larger transfers (e.g. SFTP 32KB
 * writes) into packets we reassemble. Was 32KB, safe only because the
 * interactive shell never sent big client->server data; SFTP does. */
#define SSH_CHAN_MAX_PKT  2000

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
        SSHLOG("[sshd] Unsupported channel type: %.*s\n",
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

    SSHLOG("[sshd] CHANNEL_OPEN session (ch=%u win=%u maxpkt=%u)\n",
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

    SSHLOG("[sshd] pty-req: %.*s %ux%u\n",
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
                       int *stdout_wr_fd, pid_t *child_pid,
                       uint32_t uid, uint32_t gid)
{
    int in_pipe[2], out_pipe[2];

    if (pipe2(in_pipe, 0) < 0) {
        SSHLOG("[sshd] stdin pipe2 failed\n");
        return -1;
    }
    if (pipe2(out_pipe, 0) < 0) {
        SSHLOG("[sshd] stdout pipe2 failed\n");
        close(in_pipe[0]);
        close(in_pipe[1]);
        return -1;
    }

    SSHLOG("[sshd] pipes: in=[%d,%d] out=[%d,%d]\n",
           in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]);

    pid_t pid = fork();
    if (pid < 0) {
        SSHLOG("[sshd] fork failed\n");
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child is root (inherited from sshd, which stays root); drop to the
         * authenticated session identity before exec. The exec'd shell keeps
         * this uid (pipe_server preserves the slot uid across PIPE_EXEC), and
         * sshd itself stays root so every session sets identity correctly
         * (v0.4.190 -- mirrors the getty restructure). */
        ssh_drop_identity(uid, gid);
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

    /* Parent: drop BOTH ends the child uses -- the stdin read end AND the
     * stdout write end -- right after fork. This is the post-v0.4.143 pipe
     * model, the same as netconsole: a pipe read end reports EOF only when
     * the REGISTERED writer (the exec'd dash, given its own write_ref by
     * PIPE_EXEC) exits, never on a plain close() of an inherited copy. So the
     * stdout read end now EOFs precisely when dash exits.
     *   The OLD code kept out_pipe[1] open and closed it only after the first
     * shell output (with an ever_got_output guard). On the A72 that later
     * close raced dash output (pipe-SHM cache lag) -> a spurious read()==0 ->
     * a false "shell exited" -> the channel was torn down after ONE command.
     * QEMU has no such lag, so it never reproduced. (stdout_wr_fd kept as a
     * param but returned -1 so the relay cleanup is a no-op.) */
    close(in_pipe[0]);
    close(out_pipe[1]);
    *stdin_wr    = in_pipe[1];
    *stdout_rd   = out_pipe[0];
    *stdout_wr_fd = -1;
    *child_pid   = pid;

    SSHLOG("[sshd] shell spawned pid=%d stdin_wr=%d stdout_rd=%d\n",
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
            /* Ctrl-C: send SIGINT to foreground process.
             * v0.4.87: kill(0,2) delivers SIGINT via pipe_server,
             * which wakes blocked PIPE_READ with EINTR. No need
             * to write raw 0x03 to stdin (stale bytes confuse dash). */
            kill(0, 2);  /* SIGINT to fg process via pipe_server */
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
    int shell_eof = 0;  /* v0.4.178: set when the shell closes stdout (real exit) */
    uint8_t rbuf[900];
    uint8_t pkt[SSH_BUF_SIZE];
    int plen;

    /* Line buffer for server-side echo */
    uint8_t lbuf[LINE_BUF_SIZE];
    int lpos = 0;

    SSHLOG("[sshd] relay loop starting\n");

    while (!done) {
        int activity = 0;
        /* 1. Read shell output (non-blocking pipe read) */
        int n = (int)read(stdout_rd, rbuf, sizeof(rbuf));
        if (n > 0) {
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
        } else if (n == 0) {
            /* Authoritative EOF: dash (the only registered writer, after we
             * closed our write end at fork) has exited. No ever_got_output
             * guard needed -- a no-output command now closes cleanly too. */
            SSHLOG("[sshd] shell exited (pipe EOF)\n");
            shell_eof = 1;
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
                    SSHLOG("[sshd] client CHANNEL_EOF\n");
                    if (stdin_wr >= 0) {
                        close(stdin_wr);
                        stdin_wr = -1;
                    }

                } else if (mtype == SSH_MSG_CHANNEL_CLOSE) {
                    SSHLOG("[sshd] client CHANNEL_CLOSE\n");
                    done = 1;

                } else if (mtype == SSH_MSG_DISCONNECT) {
                    SSHLOG("[sshd] client disconnected\n");
                    done = 1;

                } else if (mtype == SSH_MSG_IGNORE ||
                           mtype == SSH_MSG_DEBUG) {
                    /* skip */
                } else {
                    SSHLOG("[sshd] relay: unhandled msg %d\n", mtype);
                }

            } else if (rc < 0) {
                SSHLOG("[sshd] socket read error in relay\n");
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
     * then close read end. */
    if (stdin_wr >= 0) close(stdin_wr);
    if (stdout_wr_fd >= 0) close(stdout_wr_fd);
    close(stdout_rd);

    /* Reap the child -- self-heal (v0.4.178).
     *
     * If the loop ended on shell EOF (shell_eof) the shell has already exited
     * and waitpid returns at once. But if it ended on a client disconnect /
     * CHANNEL_CLOSE / socket error, the shell may still be running -- and could
     * be HUNG (e.g. zsh wedged its raw-mode ZLE over the cooked SSH relay). A
     * plain blocking waitpid would then hang sshd forever, taking the whole
     * one-connection-at-a-time server down for EVERY future client (the wedge
     * that needed a netconsole kill / reboot to clear). So when the shell has
     * not signalled EOF, SIGKILL it first -- pipe_server reaps it via the fault
     * path and creates the zombie, so the waitpid below returns immediately.
     * One hung shell can no longer wedge the service.
     *   (A grandchild the shell itself spawned -- e.g. zsh under dash -- is
     * orphaned rather than recursively killed; rare leak, tracked in BACKLOG.) */
    if (!shell_eof) {
        SSHLOG("[sshd] client gone with shell still alive -- killing pid %d\n",
               (int)child);
        kill(child, SIGKILL);
    }
    int status = 0;
    waitpid(child, &status, 0);
    SSHLOG("[sshd] shell reaped (status=%d eof=%d)\n", status, shell_eof);

    SSHLOG("[sshd] relay loop ended\n");
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
            SSHLOG("[sshd] disconnect before channel open\n");
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
        SSHLOG("[sshd] expected CHANNEL_OPEN, got %d\n", mtype);
        return -1;
    }

    /* ---- Handle channel requests until "shell" or "subsystem sftp" ---- */
    int shell_requested = 0;
    int sftp_requested = 0;
    while (!shell_requested && !sftp_requested) {
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
            SSHLOG("[sshd] expected CHANNEL_REQUEST, got %d\n", mtype);
            continue;
        }

        /* Parse CHANNEL_REQUEST */
        int off = 1 + 4;  /* skip type + recipient channel */
        const uint8_t *rtype;
        uint32_t rtype_len;
        if (ssh_get_string(pkt, plen, &off, &rtype, &rtype_len) < 0) {
            SSHLOG("[sshd] bad CHANNEL_REQUEST format\n");
            return -1;
        }
        int want_reply = 0;
        if (off < plen) want_reply = pkt[off++];

        SSHLOG("[sshd] CHANNEL_REQUEST: %.*s (reply=%d)\n",
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

        } else if (rtype_len == 9 && memcmp(rtype, "subsystem", 9) == 0) {
            /* v0.4.178: only the "sftp" subsystem is supported. */
            const uint8_t *sub; uint32_t sublen;
            if (ssh_get_string(pkt, plen, &off, &sub, &sublen) == 0
                && sublen == 4 && memcmp(sub, "sftp", 4) == 0) {
                SSHLOG("[sshd] subsystem: sftp\n");
                sftp_requested = 1;
                if (want_reply) { if (send_chan_success(s) < 0) return -1; }
            } else {
                SSHLOG("[sshd] unsupported subsystem: %.*s\n",
                       (int)sublen, (const char *)sub);
                if (want_reply) send_chan_failure(s);
            }

        } else if (rtype_len == 4 && memcmp(rtype, "exec", 4) == 0) {
            SSHLOG("[sshd] exec not yet supported\n");
            if (want_reply) send_chan_failure(s);

        } else {
            SSHLOG("[sshd] unknown request: %.*s\n",
                   (int)rtype_len, (const char *)rtype);
            if (want_reply) send_chan_failure(s);
        }
    }

    /* ---- SFTP subsystem: serve files directly over the channel (no shell,
     * no pipe relay -- so the A72 shell-pipe drain race does not apply). ---- */
    if (sftp_requested) {
        int rc = ssh_do_sftp(s);
        s->channel_open = 0;
        return rc;
    }

    /* ---- Spawn shell and enter relay ---- */
    int stdin_wr = -1, stdout_rd = -1, stdout_wr_fd = -1;
    pid_t child_pid = -1;

    if (spawn_shell(&stdin_wr, &stdout_rd, &stdout_wr_fd, &child_pid,
                    s->uid, s->gid) < 0) {
        SSHLOG("[sshd] shell spawn failed\n");
        send_chan_close(s);
        return -1;
    }

    int rc = channel_relay(s, stdin_wr, stdout_rd, stdout_wr_fd, child_pid);
    s->channel_open = 0;
    return rc;
}

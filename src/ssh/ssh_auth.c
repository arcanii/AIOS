/* ssh_auth.c -- SSH user authentication (RFC 4252)
 *
 * Handles USERAUTH_REQUEST for "none" and "password" methods.
 * Password verification via AUTH_LOGIN IPC to auth_server
 * (same protocol as getty).
 *
 * After successful login, calls PIPE_SET_IDENTITY to propagate
 * uid/gid to child processes.
 */

#include "ssh_session.h"
#include "aios_posix.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* IPC labels (match getty.c / pipe_server.c) */
#define AUTH_LOGIN        40
#define AUTH_LOGOUT       41
#define PIPE_SET_IDENTITY 74

/* Max login attempts before disconnect */
#define MAX_AUTH_ATTEMPTS 3

/* ----------------------------------------------------------------
 * Failed-attempt backoff (v0.4.187, anti-brute-force)
 *
 * Delay (2 * attempt + prior failures) seconds, capped at 10, before
 * each USERAUTH_FAILURE for a counted attempt. A fresh boot sees
 * 2s/5s/8s across the three attempts of one connection; a reconnect
 * starts at 2s + 1s per prior failure (sshd is one process per boot,
 * so the static counter persists -- reconnecting for a fresh
 * 3-attempt window no longer resets the cost). The cap keeps a
 * legitimate typo tolerable; the counter saturates at 8 because the
 * cap makes higher values indistinguishable. Successful first-try
 * logins (and the method=none probe) see no delay at all.
 * NOTE: nanosleep here is a yield busy-wait (the platform sleep
 * primitive), so the tarpit spends sshd thread time, not just the
 * time of the attacking client.
 * ---------------------------------------------------------------- */

static int g_prior_failures;

static void auth_fail_delay(int attempt)
{
    int secs = 2 * attempt + g_prior_failures;
    if (secs > 10) secs = 10;
    struct timespec ts = { .tv_sec = secs, .tv_nsec = 0 };
    nanosleep(&ts, NULL);
}

/* ----------------------------------------------------------------
 * Pack a string into seL4 message registers (8 bytes per MR)
 *
 * Same encoding as getty: MR[start] = length, then data packed
 * little-endian, 8 bytes per seL4_Word.
 * Returns the next MR index after the string.
 * ---------------------------------------------------------------- */

static int pack_string_mr(int start_mr, const char *str, int len)
{
    int mr = start_mr;
    seL4_SetMR(mr++, (seL4_Word)len);
    seL4_Word w = 0;
    int i;
    for (i = 0; i < len; i++) {
        w |= ((seL4_Word)(uint8_t)str[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == len - 1) {
            seL4_SetMR(mr++, w);
            w = 0;
        }
    }
    return mr;
}

/* ----------------------------------------------------------------
 * Call AUTH_LOGIN IPC to auth_server
 *
 * Returns 0 on success (uid/gid/token filled in), -1 on failure.
 * ---------------------------------------------------------------- */

static int auth_login(const char *user, int ulen,
                       const char *pass, int plen,
                       uint32_t *uid, uint32_t *gid, uint32_t *token)
{
    seL4_CPtr auth_ep = aios_get_auth_ep();
    if (!auth_ep) {
        /* No auth server -- auto-login as root */
        SSHLOG("[sshd] No auth_ep, auto-login as root\n");
        *uid = 0;
        *gid = 0;
        *token = 1;
        return 0;
    }

    /* Pack username + password into MRs */
    int mr = 0;
    mr = pack_string_mr(mr, user, ulen);
    mr = pack_string_mr(mr, pass, plen);

    seL4_MessageInfo_t reply = seL4_Call(auth_ep,
        seL4_MessageInfo_new(AUTH_LOGIN, 0, 0, mr));
    (void)reply;

    uint32_t status = (uint32_t)seL4_GetMR(0);
    if (status != 0) {
        return -1;
    }

    *uid   = (uint32_t)seL4_GetMR(1);
    *gid   = (uint32_t)seL4_GetMR(2);
    *token = (uint32_t)seL4_GetMR(3);
    return 0;
}

/* ----------------------------------------------------------------
 * Release the auth_server session for this connection.
 *
 * v0.4.178: the SSH reconnect fix (part 2). auth_server has only
 * AIOS_AUTH_MAX_SESSIONS (4) session slots; one is the permanent boot-root
 * session. sshd took a slot per login via AUTH_LOGIN but never released it,
 * so after a few connections per boot the table filled and login returned
 * NOSLOT -> the client saw "Permission denied (password)". Freeing the slot
 * when the connection ends keeps sequential logins working indefinitely.
 * token 0 = never authenticated; token 1 = auth-server-less auto-login (no
 * slot to free). Both are skipped (the latter also via the !auth_ep guard).
 * ---------------------------------------------------------------- */
void ssh_auth_logout(uint32_t token)
{
    if (token == 0 || token == 1) return;
    seL4_CPtr auth_ep = aios_get_auth_ep();
    if (!auth_ep) return;
    seL4_SetMR(0, (seL4_Word)token);
    seL4_Call(auth_ep, seL4_MessageInfo_new(AUTH_LOGOUT, 0, 0, 1));
}

/* ----------------------------------------------------------------
 * Call PIPE_SET_IDENTITY to propagate uid/gid
 * ---------------------------------------------------------------- */

static void set_identity(uint32_t uid, uint32_t gid)
{
    seL4_CPtr pipe_ep = aios_get_pipe_ep();
    if (!pipe_ep) return;

    seL4_SetMR(0, (seL4_Word)uid);
    seL4_SetMR(1, (seL4_Word)gid);
    seL4_Call(pipe_ep,
        seL4_MessageInfo_new(PIPE_SET_IDENTITY, 0, 0, 2));
}

/* ----------------------------------------------------------------
 * Send USERAUTH_FAILURE
 *
 * name-list: methods that can continue
 * boolean:   partial success (always FALSE)
 * ---------------------------------------------------------------- */

static int send_auth_failure(ssh_session_t *s)
{
    uint8_t payload[32];
    int off = 0;

    payload[off++] = SSH_MSG_USERAUTH_FAILURE;
    ssh_put_namelist(payload, "password", &off);
    payload[off++] = 0;  /* partial success = FALSE */

    return ssh_write_packet(s, payload, off);
}

/* ----------------------------------------------------------------
 * Send USERAUTH_SUCCESS
 * ---------------------------------------------------------------- */

static int send_auth_success(ssh_session_t *s)
{
    uint8_t payload[1];
    payload[0] = SSH_MSG_USERAUTH_SUCCESS;
    return ssh_write_packet(s, payload, 1);
}

/* ----------------------------------------------------------------
 * Main user authentication loop (RFC 4252)
 *
 * OpenSSH flow:
 *   1. Client sends USERAUTH_REQUEST method="none" (probe)
 *   2. Server responds USERAUTH_FAILURE listing "password"
 *   3. Client sends USERAUTH_REQUEST method="password"
 *   4. Server verifies via AUTH_LOGIN IPC
 *   5. Server responds SUCCESS or FAILURE
 *
 * Returns 0 on successful authentication, -1 on failure.
 * ---------------------------------------------------------------- */

int ssh_do_userauth(ssh_session_t *s)
{
    int attempts = 0;
    int rounds = 0;

    while (attempts < MAX_AUTH_ATTEMPTS) {
        uint8_t pkt[SSH_BUF_SIZE];
        int plen = 0;

        /* Bound this userauth request loop (v0.4.187): method=none probes,
         * unknown methods, and IGNORE/DEBUG packets do not count as
         * attempts, so without a cap a client could loop here forever.
         * Normal flow uses 2-4 rounds (none probe + 1-3 password attempts).
         * The transport-level loops (version exchange, service request)
         * are separate and not bounded by this. */
        if (++rounds > 16) {
            SSHLOG("[sshd] Too many pre-auth requests, disconnecting\n");
            return -1;
        }

        if (ssh_read_packet(s, pkt, &plen) < 0) {
            SSHLOG("[sshd] Auth packet read failed\n");
            return -1;
        }
        if (plen < 1) return -1;

        uint8_t mtype = pkt[0];

        /* Handle non-auth messages that may arrive */
        if (mtype == SSH_MSG_IGNORE || mtype == SSH_MSG_DEBUG) {
            continue;
        }
        if (mtype == SSH_MSG_DISCONNECT) {
            SSHLOG("[sshd] Client disconnected during auth\n");
            return -1;
        }

        if (mtype != SSH_MSG_USERAUTH_REQUEST) {
            SSHLOG("[sshd] Expected USERAUTH_REQUEST (50), got %d\n", mtype);
            return -1;
        }

        /* Parse USERAUTH_REQUEST:
         *   string  user name
         *   string  service name
         *   string  method name
         *   [method-specific fields]
         */
        int off = 1;
        const uint8_t *user_data;
        uint32_t user_len;
        const uint8_t *svc_data;
        uint32_t svc_len;
        const uint8_t *method_data;
        uint32_t method_len;

        if (ssh_get_string(pkt, plen, &off, &user_data, &user_len) < 0 ||
            ssh_get_string(pkt, plen, &off, &svc_data, &svc_len) < 0 ||
            ssh_get_string(pkt, plen, &off, &method_data, &method_len) < 0) {
            SSHLOG("[sshd] Bad USERAUTH_REQUEST format\n");
            return -1;
        }

        /* Copy username to session (null-terminate, cap at 31 chars) */
        char username[32];
        int ulen = (int)user_len;
        if (ulen > 31) ulen = 31;
        memcpy(username, user_data, ulen);
        username[ulen] = '\0';

        SSHLOG("[sshd] USERAUTH: user=%s service=%.*s method=%.*s\n",
               username, (int)svc_len, (const char *)svc_data,
               (int)method_len, (const char *)method_data);

        /* Method: "none" -- probe for available methods */
        if (method_len == 4 && memcmp(method_data, "none", 4) == 0) {
            SSHLOG("[sshd] Auth method 'none' -- sending available methods\n");
            if (send_auth_failure(s) < 0) return -1;
            continue;
        }

        /* Method: "password" */
        if (method_len == 8 && memcmp(method_data, "password", 8) == 0) {
            /* Parse password field:
             *   boolean  FALSE (not changing password)
             *   string   password
             */
            if (off >= plen) {
                SSHLOG("[sshd] Password field missing\n");
                auth_fail_delay(attempts + 1);
                if (g_prior_failures < 8) g_prior_failures++;
                if (send_auth_failure(s) < 0) return -1;
                attempts++;
                continue;
            }

            /* Skip boolean (1 byte, should be FALSE) */
            off++;

            const uint8_t *pass_data;
            uint32_t pass_len;
            if (ssh_get_string(pkt, plen, &off, &pass_data, &pass_len) < 0) {
                SSHLOG("[sshd] Bad password string\n");
                auth_fail_delay(attempts + 1);
                if (g_prior_failures < 8) g_prior_failures++;
                if (send_auth_failure(s) < 0) return -1;
                attempts++;
                continue;
            }

            /* Copy password to local buffer for IPC (null-terminate) */
            char password[128];
            int pwlen = (int)pass_len;
            if (pwlen > 127) pwlen = 127;
            memcpy(password, pass_data, pwlen);
            password[pwlen] = '\0';

            /* Authenticate via AUTH_LOGIN IPC */
            uint32_t uid = 0, gid = 0, token = 0;
            int rc = auth_login(username, ulen, password, pwlen,
                                &uid, &gid, &token);

            /* Scrub password from stack */
            memset(password, 0, sizeof(password));

            if (rc == 0) {
                SSHLOG("[sshd] Auth OK: uid=%u gid=%u token=0x%x\n",
                       (unsigned)uid, (unsigned)gid, (unsigned)token);

                /* Propagate identity to pipe_server */
                set_identity(uid, gid);

                /* Store in session */
                s->uid = uid;
                s->gid = gid;
                s->auth_token = token;
                s->authenticated = 1;

                if (send_auth_success(s) < 0) return -1;

                SSHLOG("[sshd] USERAUTH_SUCCESS sent\n");
                return 0;
            }

            SSHLOG("[sshd] Auth FAILED for user %s (attempt %d)\n",
                   username, attempts + 1);
            auth_fail_delay(attempts + 1);
            if (g_prior_failures < 8) g_prior_failures++;
            if (send_auth_failure(s) < 0) return -1;
            attempts++;
            continue;
        }

        /* Unknown method -- respond with available methods */
        SSHLOG("[sshd] Unknown auth method: %.*s\n",
               (int)method_len, (const char *)method_data);
        if (send_auth_failure(s) < 0) return -1;
    }

    SSHLOG("[sshd] Too many auth failures, disconnecting\n");
    return -1;
}

/*
 * AIOS Auth Server — Protocol & Types
 *
 * Authentication and authorization via seL4 IPC.
 * SHA-3-512 (Keccak) password hashing.
 * Session-based access control.
 */
#ifndef AIOS_AUTH_H
#define AIOS_AUTH_H

#include <stdint.h>

/* ── IPC Labels (40–51) ── */
#define AIOS_AUTH_LOGIN       40
#define AIOS_AUTH_LOGOUT      41
#define AIOS_AUTH_WHOAMI      42
#define AIOS_AUTH_CHECK_FILE  43
#define AIOS_AUTH_CHECK_KILL  44
#define AIOS_AUTH_CHECK_PRIV  45
#define AIOS_AUTH_USERADD     46
#define AIOS_AUTH_PASSWD      47
#define AIOS_AUTH_SU          48
#define AIOS_AUTH_GROUPS      49
#define AIOS_AUTH_USERMOD     50
#define AIOS_AUTH_GET_USER    51  /* lookup uid → username, home, shell */

/* ── Access modes for CHECK_FILE ── */
#define AIOS_ACCESS_READ   0x04
#define AIOS_ACCESS_WRITE  0x02
#define AIOS_ACCESS_EXEC   0x01

/* ── Status codes ── */
#define AIOS_AUTH_OK           0
#define AIOS_AUTH_ERR_DENIED  ((uint32_t)-1)
#define AIOS_AUTH_ERR_NOSLOT  ((uint32_t)-2)
#define AIOS_AUTH_ERR_EXISTS  ((uint32_t)-3)

/* ── User database ── */
#define AIOS_AUTH_MAX_USERS          16
#define AIOS_AUTH_MAX_GROUPS_PER_USER 16
#define AIOS_AUTH_MAX_SESSIONS        4

typedef struct {
    int      active;
    char     username[32];
    char     passhash[129];  /* SHA-3-512: 128 hex chars + NUL */
    uint32_t uid;
    uint32_t gid;
    uint32_t groups[AIOS_AUTH_MAX_GROUPS_PER_USER];
    int      ngroups;
    int      is_root;
    char     home[64];
    char     shell[64];
    char     gecos[64];
} aios_user_t;

typedef struct {
    int      active;
    uint32_t token;
    uint32_t uid;
    uint32_t gid;
    uint32_t groups[AIOS_AUTH_MAX_GROUPS_PER_USER];
    int      ngroups;
    int      is_root;
    char     username[32];
} aios_session_t;

/* ── API (implemented in aios_auth.c) ── */

/* Thread entry point — pass auth_ep as arg0 */
void aios_auth_thread_fn(void *arg0, void *arg1, void *ipc_buf);

/* Init user database (called before thread start) */
void aios_auth_init(void);

/* Load /etc/passwd from VFS into user database */
int aios_auth_load_passwd(void);

/* Auto-login root — returns session token */
uint32_t aios_auth_login_root(void);

/* SHA-3-512 (exposed for testing) */
void aios_sha3_512(const uint8_t *data, uint32_t len, uint8_t hash[64]);

#endif /* AIOS_AUTH_H */

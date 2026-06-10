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
#define AIOS_AUTH_GET_USER    51
#define AIOS_AUTH_LOAD_PASSWD 52  /* lookup uid → username, home, shell */

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

/* ── Password KDF (v0.4.189) ──
 * Stored hash format: "$a1$<salt_hex>$<derived_hex>"
 *   salt = AIOS_KDF_SALT_BYTES random bytes (per-user, defeats rainbow tables)
 *   derived = SHA3-512 iterated AIOS_KDF_ITERS times over (h || salt || pass)
 * This replaces the old bare single SHA3-512 (unsalted, no work factor).
 * AIOS_KDF_ITERS MUST match scripts/gen_etc_passwd.py (kept in sync; the script
 * parses this header). It is a research-OS work factor, not bcrypt-strength. */
#define AIOS_KDF_SALT_BYTES  8
#define AIOS_KDF_ITERS       12000
#define AIOS_PASSHASH_LEN    160   /* "$a1$" + 16 salt-hex + "$" + 128 hash-hex + NUL */

typedef struct {
    int      active;
    char     username[32];
    char     passhash[AIOS_PASSHASH_LEN];  /* "$a1$salt$hash" (v0.4.189) */
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

/* The auth server is the standalone MMU-isolated process src/apps/auth_server.c
 * (a BOOT_APP). It has no exported C API -- everything is via the IPC labels
 * above. This header is the shared protocol/type definition only. */

#endif /* AIOS_AUTH_H */

/*
 * Open Aries — Auth IPC protocol
 *
 * Shared between orchestrator and auth_server.
 */
#ifndef AIOS_AUTH_H
#define AIOS_AUTH_H

/* ── Shared memory layout ──────────────────────────── */
#define AUTH_CMD        0x000
#define AUTH_STATUS     0x004
#define AUTH_UID        0x008
#define AUTH_GID        0x00C
#define AUTH_SESSION    0x010
#define AUTH_USERNAME   0x040
#define AUTH_PASSWORD   0x080
#define AUTH_PATH       0x0C0
#define AUTH_MODE       0x100
#define AUTH_DATA       0x200  /* passwd file contents (up to 3584 bytes) */
#define AUTH_DATA_MAX   3584

/* ── Commands ──────────────────────────────────────── */
#define AUTH_CMD_LOGIN      1
#define AUTH_CMD_CHECK_FILE 2
#define AUTH_CMD_CHECK_KILL 3
#define AUTH_CMD_CHECK_PRIV 4
#define AUTH_CMD_LOGOUT     5
#define AUTH_CMD_WHOAMI     7
#define AUTH_CMD_USERADD    8
#define AUTH_CMD_LOAD_PASSWD 9  /* orchestrator sends /etc/passwd contents */
#define AUTH_CMD_PASSWD     10  /* change password */
#define AUTH_CMD_SU         11  /* switch user */

/* ── Access modes for CHECK_FILE ───────────────────── */
#define ACCESS_READ     0x04
#define ACCESS_WRITE    0x02
#define ACCESS_EXEC     0x01

#endif /* AIOS_AUTH_H */

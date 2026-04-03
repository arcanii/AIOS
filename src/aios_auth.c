/*
 * AIOS Auth Server — SHA-3-512 Authentication
 *
 * Runs as a thread in root's VSpace. Manages user database,
 * sessions, and access control via seL4 IPC.
 */
#include "aios/aios_auth.h"
#include "aios/vfs.h"
#include <sel4/sel4.h>
#include <stdint.h>
#include <stdio.h>

#define LOG_MODULE "auth"
#define LOG_LEVEL  LOG_LEVEL_INFO
#include "aios/aios_log.h"

/* ═══════════════════════════════════════════════════════
 *  String helpers (no libc dependency in root VSpace)
 * ═══════════════════════════════════════════════════════ */
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void str_cpy(char *d, const char *s) {
    while ((*d++ = *s++));
}

static int str_cmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void mem_zero(void *p, int n) {
    unsigned char *b = (unsigned char *)p;
    for (int i = 0; i < n; i++) b[i] = 0;
}

/* ═══════════════════════════════════════════════════════
 *  SHA-3-512 (Keccak-f[1600])
 *
 *  Sponge construction: rate=576 bits (72 bytes),
 *  capacity=1024 bits, output=512 bits (64 bytes).
 *  Domain separation byte: 0x06 (SHA-3).
 * ═══════════════════════════════════════════════════════ */

#define ROL64(x, n) (((x) << (n)) | ((x) >> (64 - (n))))

static const uint64_t KECCAK_RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL,
    0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL,
    0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL,
    0x0000000080000001ULL, 0x8000000080008008ULL,
};

static const int KECCAK_ROTC[24] = {
     1,  3,  6, 10, 15, 21, 28, 36,
    45, 55,  2, 14, 27, 41, 56,  8,
    25, 43, 62, 18, 39, 61, 20, 44
};

static const int KECCAK_PILN[24] = {
    10,  7, 11, 17, 18,  3,  5, 16,
     8, 21, 24,  4, 15, 23, 19, 13,
    12,  2, 20, 14, 22,  9,  6,  1
};

static void keccakf(uint64_t st[25]) {
    for (int round = 0; round < 24; round++) {
        /* theta */
        uint64_t bc[5];
        for (int i = 0; i < 5; i++)
            bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
        for (int i = 0; i < 5; i++) {
            uint64_t t = bc[(i + 4) % 5] ^ ROL64(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5)
                st[j + i] ^= t;
        }
        /* rho + pi */
        uint64_t t = st[1];
        for (int i = 0; i < 24; i++) {
            int j = KECCAK_PILN[i];
            uint64_t tmp = st[j];
            st[j] = ROL64(t, KECCAK_ROTC[i]);
            t = tmp;
        }
        /* chi */
        for (int j = 0; j < 25; j += 5) {
            uint64_t tmp[5];
            for (int i = 0; i < 5; i++) tmp[i] = st[j + i];
            for (int i = 0; i < 5; i++)
                st[j + i] = tmp[i] ^ ((~tmp[(i + 1) % 5]) & tmp[(i + 2) % 5]);
        }
        /* iota */
        st[0] ^= KECCAK_RC[round];
    }
}

/* Load 8 bytes as little-endian uint64 */
static uint64_t le64(const uint8_t *p) {
    return (uint64_t)p[0]       | ((uint64_t)p[1] << 8)  |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

/* Store uint64 as little-endian 8 bytes */
static void st64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

void aios_sha3_512(const uint8_t *data, uint32_t len, uint8_t hash[64]) {
    uint64_t st[25];
    mem_zero(st, sizeof(st));
    int rate = 72;  /* SHA-3-512: rate = 576 bits = 72 bytes */

    /* Absorb full blocks */
    uint32_t pos = 0;
    while (pos + (uint32_t)rate <= len) {
        for (int i = 0; i < rate / 8; i++)
            st[i] ^= le64(&data[pos + i * 8]);
        keccakf(st);
        pos += (uint32_t)rate;
    }

    /* Final block with SHA-3 padding */
    uint8_t pad[72];
    mem_zero(pad, rate);
    uint32_t remaining = len - pos;
    for (uint32_t i = 0; i < remaining; i++)
        pad[i] = data[pos + i];
    pad[remaining] = 0x06;           /* SHA-3 domain separation */
    pad[rate - 1] |= 0x80;           /* final padding bit */
    for (int i = 0; i < rate / 8; i++)
        st[i] ^= le64(&pad[i * 8]);
    keccakf(st);

    /* Squeeze: 64 bytes output (8 x uint64), fits in one block (64 < 72) */
    for (int i = 0; i < 8; i++)
        st64(&hash[i * 8], st[i]);
}

/* Convert 64-byte hash to 128-char hex string */
static void hash_to_hex(const uint8_t hash[64], char hex[129]) {
    const char *digits = "0123456789abcdef";
    for (int i = 0; i < 64; i++) {
        hex[i * 2]     = digits[hash[i] >> 4];
        hex[i * 2 + 1] = digits[hash[i] & 0x0f];
    }
    hex[128] = '\0';
}

/* ═══════════════════════════════════════════════════════
 *  User database
 * ═══════════════════════════════════════════════════════ */
static aios_user_t    users[AIOS_AUTH_MAX_USERS];
static int            num_users = 0;
static aios_session_t sessions[AIOS_AUTH_MAX_SESSIONS];
static uint32_t       next_token = 0x1001;

static void init_default_users(void) {
    mem_zero(users, sizeof(users));
    mem_zero(sessions, sizeof(sessions));

    /* root:root (SHA-3-512) */
    users[0].active = 1;
    str_cpy(users[0].username, "root");
    str_cpy(users[0].passhash,
        "8cd824c700eb0c125fff40c8c185d14c5dfe7f32814afac079ba7c20d93bc3c0"
        "82193243c420fed22ef2474fbb85880e7bc1ca772150a1f759f8ddebca77711f");
    users[0].uid = 0;
    users[0].gid = 0;
    users[0].ngroups = 0;
    users[0].is_root = 1;
    str_cpy(users[0].home, "/root");
    str_cpy(users[0].shell, "/bin/sh");
    str_cpy(users[0].gecos, "System Administrator");

    /* user:user (SHA-3-512) */
    users[1].active = 1;
    str_cpy(users[1].username, "user");
    str_cpy(users[1].passhash,
        "dee4164777a98291e138fcebcf7ea59a837226bc8388cd1cf694581586910a81"
        "d46f07b93c068f17eae5a8337201af7d51b3a888a6db41915d801cb15b6058e5");
    users[1].uid = 1000;
    users[1].gid = 1000;
    users[1].ngroups = 0;
    users[1].is_root = 0;
    str_cpy(users[1].home, "/home/user");
    str_cpy(users[1].shell, "/bin/sh");
    str_cpy(users[1].gecos, "Regular User");

    num_users = 2;
}

/* ═══════════════════════════════════════════════════════
 *  Session helpers
 * ═══════════════════════════════════════════════════════ */
static aios_session_t *find_session(uint32_t token) {
    for (int i = 0; i < AIOS_AUTH_MAX_SESSIONS; i++) {
        if (sessions[i].active && sessions[i].token == token)
            return &sessions[i];
    }
    return 0;
}

static int user_in_group(aios_session_t *s, uint32_t gid) {
    if (s->gid == gid) return 1;
    for (int i = 0; i < s->ngroups; i++)
        if (s->groups[i] == gid) return 1;
    return 0;
}

/* ═══════════════════════════════════════════════════════
 *  IPC string helpers — pack/unpack via seL4 MRs
 * ═══════════════════════════════════════════════════════ */
static int unpack_string(int start_mr, char *buf, int maxlen) {
    seL4_Word slen = seL4_GetMR(start_mr);
    int len = (int)slen;
    if (len > maxlen - 1) len = maxlen - 1;
    int mr = start_mr + 1;
    for (int i = 0; i < len; i++) {
        if (i % 8 == 0 && i > 0) mr++;
        buf[i] = (char)((seL4_GetMR(mr) >> ((i % 8) * 8)) & 0xFF);
    }
    buf[len] = '\0';
    /* Return next MR index after this string */
    return start_mr + 1 + (len + 7) / 8;
}

static int pack_string(int start_mr, const char *str) {
    int len = str_len(str);
    seL4_SetMR(start_mr, (seL4_Word)len);
    int mr = start_mr + 1;
    seL4_Word w = 0;
    for (int i = 0; i < len; i++) {
        w |= ((seL4_Word)(uint8_t)str[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == len - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    return mr;
}

/* ═══════════════════════════════════════════════════════
 *  Command handlers
 * ═══════════════════════════════════════════════════════ */

/* LOGIN: MR0=uname_len, MR1..=uname, then pass_len, pass
 * Reply: MR0=status, MR1=uid, MR2=gid, MR3=token */
static void handle_login(void) {
    char u[32], p[64];
    int next_mr = unpack_string(0, u, 32);
    unpack_string(next_mr, p, 64);

    /* Hash password */
    uint8_t hash[64];
    aios_sha3_512((const uint8_t *)p, (uint32_t)str_len(p), hash);
    char hex[129];
    hash_to_hex(hash, hex);
    mem_zero(p, 64);  /* clear plaintext */

    /* Find matching user */
    for (int i = 0; i < num_users; i++) {
        if (!users[i].active) continue;
        if (str_cmp(users[i].username, u) == 0 &&
            str_cmp(users[i].passhash, hex) == 0) {
            /* Create session */
            for (int si = 0; si < AIOS_AUTH_MAX_SESSIONS; si++) {
                if (!sessions[si].active) {
                    aios_session_t *s = &sessions[si];
                    s->active = 1;
                    s->token = next_token++;
                    s->uid = users[i].uid;
                    s->gid = users[i].gid;
                    s->ngroups = users[i].ngroups;
                    for (int g = 0; g < users[i].ngroups; g++)
                        s->groups[g] = users[i].groups[g];
                    s->is_root = users[i].is_root;
                    str_cpy(s->username, users[i].username);

                    AIOS_LOG_INFO("login OK");
                    seL4_SetMR(0, AIOS_AUTH_OK);
                    seL4_SetMR(1, (seL4_Word)s->uid);
                    seL4_SetMR(2, (seL4_Word)s->gid);
                    seL4_SetMR(3, (seL4_Word)s->token);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 4));
                    return;
                }
            }
            seL4_SetMR(0, AIOS_AUTH_ERR_NOSLOT);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            return;
        }
    }
    AIOS_LOG_WARN("login DENIED");
    seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
}

/* LOGOUT: MR0=token → MR0=status */
static void handle_logout(void) {
    uint32_t token = (uint32_t)seL4_GetMR(0);
    aios_session_t *s = find_session(token);
    if (s) {
        AIOS_LOG_INFO("logout");
        s->active = 0;
        seL4_SetMR(0, AIOS_AUTH_OK);
    } else {
        seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
    }
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
}

/* WHOAMI: MR0=token → MR0=status, MR1=uid, MR2=gid, MR3..=username */
static void handle_whoami(void) {
    uint32_t token = (uint32_t)seL4_GetMR(0);
    aios_session_t *s = find_session(token);
    if (!s) {
        seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        return;
    }
    seL4_SetMR(0, AIOS_AUTH_OK);
    seL4_SetMR(1, (seL4_Word)s->uid);
    seL4_SetMR(2, (seL4_Word)s->gid);
    int last_mr = pack_string(3, s->username);
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, last_mr));
}

/* CHECK_FILE: MR0=token, MR1=file_uid, MR2=file_gid, MR3=file_mode, MR4=access */
static void handle_check_file(void) {
    uint32_t token     = (uint32_t)seL4_GetMR(0);
    uint32_t file_uid  = (uint32_t)seL4_GetMR(1);
    uint32_t file_gid  = (uint32_t)seL4_GetMR(2);
    uint16_t file_mode = (uint16_t)seL4_GetMR(3);
    uint32_t access    = (uint32_t)seL4_GetMR(4);

    aios_session_t *s = find_session(token);
    if (!s) { seL4_SetMR(0, AIOS_AUTH_ERR_DENIED); goto reply; }

    /* Root can do anything */
    if (s->is_root) { seL4_SetMR(0, AIOS_AUTH_OK); goto reply; }

    /* No file mode info — allow */
    if (file_mode == 0) { seL4_SetMR(0, AIOS_AUTH_OK); goto reply; }

    /* Unix permission check: owner → group → other */
    uint16_t perm;
    if (s->uid == file_uid)
        perm = (file_mode >> 6) & 0x7;
    else if (user_in_group(s, file_gid))
        perm = (file_mode >> 3) & 0x7;
    else
        perm = file_mode & 0x7;

    if ((access & AIOS_ACCESS_READ)  && !(perm & 0x4)) goto denied;
    if ((access & AIOS_ACCESS_WRITE) && !(perm & 0x2)) goto denied;
    if ((access & AIOS_ACCESS_EXEC)  && !(perm & 0x1)) goto denied;

    seL4_SetMR(0, AIOS_AUTH_OK);
    goto reply;
denied:
    seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
reply:
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
}

/* CHECK_KILL: MR0=token, MR1=target_uid */
static void handle_check_kill(void) {
    uint32_t token = (uint32_t)seL4_GetMR(0);
    uint32_t target = (uint32_t)seL4_GetMR(1);
    aios_session_t *s = find_session(token);
    if (s && (s->is_root || s->uid == target))
        seL4_SetMR(0, AIOS_AUTH_OK);
    else
        seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
}

/* CHECK_PRIV: MR0=token → MR0=status (0=root) */
static void handle_check_priv(void) {
    uint32_t token = (uint32_t)seL4_GetMR(0);
    aios_session_t *s = find_session(token);
    if (s && s->is_root)
        seL4_SetMR(0, AIOS_AUTH_OK);
    else
        seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
}

/* USERADD: MR0=token, MR1=uid, MR2=gid, MR3=uname_len, MR4..=uname, pass_len, pass */
static void handle_useradd(void) {
    uint32_t token = (uint32_t)seL4_GetMR(0);
    uint32_t new_uid = (uint32_t)seL4_GetMR(1);
    uint32_t new_gid = (uint32_t)seL4_GetMR(2);

    aios_session_t *s = find_session(token);
    if (!s || !s->is_root) {
        seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        return;
    }
    if (num_users >= AIOS_AUTH_MAX_USERS) {
        seL4_SetMR(0, AIOS_AUTH_ERR_NOSLOT);
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        return;
    }

    char u[32], p[64];
    int next_mr = unpack_string(3, u, 32);
    unpack_string(next_mr, p, 64);

    /* Check duplicate */
    for (int i = 0; i < num_users; i++) {
        if (users[i].active && str_cmp(users[i].username, u) == 0) {
            seL4_SetMR(0, AIOS_AUTH_ERR_EXISTS);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            return;
        }
    }

    /* Hash password */
    uint8_t hash[64];
    aios_sha3_512((const uint8_t *)p, (uint32_t)str_len(p), hash);
    char hex[129];
    hash_to_hex(hash, hex);
    mem_zero(p, 64);

    aios_user_t *nu = &users[num_users++];
    nu->active = 1;
    str_cpy(nu->username, u);
    str_cpy(nu->passhash, hex);
    nu->uid = new_uid;
    nu->gid = new_gid;
    nu->is_root = (new_uid == 0) ? 1 : 0;
    nu->ngroups = 0;
    /* Default home/shell */
    if (new_uid == 0) str_cpy(nu->home, "/root");
    else {
        str_cpy(nu->home, "/home/");
        int hi = 6;
        for (int j = 0; u[j] && hi < 63; j++) nu->home[hi++] = u[j];
        nu->home[hi] = '\0';
    }
    str_cpy(nu->shell, "/bin/sh");

    AIOS_LOG_INFO("useradd OK");
    seL4_SetMR(0, AIOS_AUTH_OK);
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
}

/* PASSWD: MR0=token, MR1=uname_len, MR2..=uname, pass_len, pass */
static void handle_passwd(void) {
    uint32_t token = (uint32_t)seL4_GetMR(0);
    aios_session_t *s = find_session(token);
    if (!s) {
        seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        return;
    }

    char u[32], p[64];
    int next_mr = unpack_string(1, u, 32);
    unpack_string(next_mr, p, 64);

    /* Only root can change other users' passwords */
    if (!s->is_root && str_cmp(s->username, u) != 0) {
        mem_zero(p, 64);
        seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        return;
    }

    for (int i = 0; i < num_users; i++) {
        if (users[i].active && str_cmp(users[i].username, u) == 0) {
            uint8_t hash[64];
            aios_sha3_512((const uint8_t *)p, (uint32_t)str_len(p), hash);
            hash_to_hex(hash, users[i].passhash);
            mem_zero(p, 64);
            AIOS_LOG_INFO("passwd changed");
            seL4_SetMR(0, AIOS_AUTH_OK);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            return;
        }
    }
    mem_zero(p, 64);
    seL4_SetMR(0, AIOS_AUTH_ERR_NOSLOT);
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
}

/* SU: MR0=token, MR1=uname_len, MR2..=uname, pass_len, pass
 * Reply: MR0=status, MR1=uid, MR2=gid */
static void handle_su(void) {
    uint32_t token = (uint32_t)seL4_GetMR(0);
    aios_session_t *s = find_session(token);
    if (!s) {
        seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        return;
    }

    char u[32], p[64];
    int next_mr = unpack_string(1, u, 32);
    unpack_string(next_mr, p, 64);

    /* Root can su without password */
    if (s->is_root && p[0] == '\0') {
        for (int i = 0; i < num_users; i++) {
            if (users[i].active && str_cmp(users[i].username, u) == 0) {
                s->uid = users[i].uid;
                s->gid = users[i].gid;
                s->is_root = users[i].is_root;
                s->ngroups = users[i].ngroups;
                for (int g = 0; g < users[i].ngroups; g++)
                    s->groups[g] = users[i].groups[g];
                str_cpy(s->username, users[i].username);
                AIOS_LOG_INFO("su OK (root)");
                seL4_SetMR(0, AIOS_AUTH_OK);
                seL4_SetMR(1, (seL4_Word)s->uid);
                seL4_SetMR(2, (seL4_Word)s->gid);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 3));
                return;
            }
        }
        seL4_SetMR(0, AIOS_AUTH_ERR_NOSLOT);
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        return;
    }

    /* Non-root: verify password */
    uint8_t hash[64];
    aios_sha3_512((const uint8_t *)p, (uint32_t)str_len(p), hash);
    char hex[129];
    hash_to_hex(hash, hex);
    mem_zero(p, 64);

    for (int i = 0; i < num_users; i++) {
        if (users[i].active && str_cmp(users[i].username, u) == 0 &&
            str_cmp(users[i].passhash, hex) == 0) {
            s->uid = users[i].uid;
            s->gid = users[i].gid;
            s->is_root = users[i].is_root;
            s->ngroups = users[i].ngroups;
            for (int g = 0; g < users[i].ngroups; g++)
                s->groups[g] = users[i].groups[g];
            str_cpy(s->username, users[i].username);
            AIOS_LOG_INFO("su OK");
            seL4_SetMR(0, AIOS_AUTH_OK);
            seL4_SetMR(1, (seL4_Word)s->uid);
            seL4_SetMR(2, (seL4_Word)s->gid);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 3));
            return;
        }
    }
    seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
}

/* GROUPS: MR0=token → MR0=count, MR1..=gids */
static void handle_groups(void) {
    uint32_t token = (uint32_t)seL4_GetMR(0);
    aios_session_t *s = find_session(token);
    if (!s) {
        seL4_SetMR(0, 0);
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        return;
    }
    int total = 1;  /* primary gid */
    seL4_SetMR(1, (seL4_Word)s->gid);
    for (int i = 0; i < s->ngroups && total < 32; i++) {
        if (s->groups[i] != s->gid) {
            seL4_SetMR(1 + total, (seL4_Word)s->groups[i]);
            total++;
        }
    }
    seL4_SetMR(0, (seL4_Word)total);
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1 + total));
}

/* USERMOD: MR0=token, MR1=target_uid, MR2=new_gid */
static void handle_usermod(void) {
    uint32_t token = (uint32_t)seL4_GetMR(0);
    uint32_t target_uid = (uint32_t)seL4_GetMR(1);
    uint32_t new_gid = (uint32_t)seL4_GetMR(2);

    aios_session_t *s = find_session(token);
    if (!s || !s->is_root) {
        seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        return;
    }

    aios_user_t *u = 0;
    for (int i = 0; i < num_users; i++) {
        if (users[i].active && users[i].uid == target_uid) { u = &users[i]; break; }
    }
    if (!u) {
        seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        return;
    }

    /* Check if already in group */
    for (int i = 0; i < u->ngroups; i++) {
        if (u->groups[i] == new_gid) {
            seL4_SetMR(0, AIOS_AUTH_OK);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            return;
        }
    }
    if (u->ngroups >= AIOS_AUTH_MAX_GROUPS_PER_USER) {
        seL4_SetMR(0, AIOS_AUTH_ERR_NOSLOT);
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        return;
    }
    u->groups[u->ngroups++] = new_gid;

    /* Update active sessions */
    for (int i = 0; i < AIOS_AUTH_MAX_SESSIONS; i++) {
        if (sessions[i].active && sessions[i].uid == target_uid)
            sessions[i].groups[sessions[i].ngroups++] = new_gid;
    }
    seL4_SetMR(0, AIOS_AUTH_OK);
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
}

/* GET_USER: MR0=uid → MR0=status, MR1=gid, MR2..=username */
static void handle_get_user(void) {
    uint32_t uid = (uint32_t)seL4_GetMR(0);
    for (int i = 0; i < num_users; i++) {
        if (users[i].active && users[i].uid == uid) {
            seL4_SetMR(0, AIOS_AUTH_OK);
            seL4_SetMR(1, (seL4_Word)users[i].gid);
            int last = pack_string(2, users[i].username);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, last));
            return;
        }
    }
    seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
}

/* ═══════════════════════════════════════════════════════
 *  /etc/passwd loader
 *
 *  Format: username:sha3hash:uid:gid:gecos:home:shell
 * ═══════════════════════════════════════════════════════ */
static int parse_uint_str(const char *s, int *pos) {
    int val = 0;
    while (s[*pos] >= '0' && s[*pos] <= '9') {
        val = val * 10 + (s[*pos] - '0');
        (*pos)++;
    }
    return val;
}

int aios_auth_load_passwd(void) {
    char buf[4096];
    int len = vfs_read("/etc/passwd", buf, sizeof(buf) - 1);
    if (len <= 0) return -1;
    buf[len] = '\0';

    /* Reset (keep defaults as fallback) */
    mem_zero(users, sizeof(users));
    num_users = 0;

    int pos = 0;
    while (buf[pos] && num_users < AIOS_AUTH_MAX_USERS) {
        if (buf[pos] == '\n') { pos++; continue; }
        if (buf[pos] == '#') {
            while (buf[pos] && buf[pos] != '\n') pos++;
            if (buf[pos] == '\n') pos++;
            continue;
        }

        aios_user_t *u = &users[num_users];
        int start, flen;

        /* Field 1: username */
        start = pos;
        while (buf[pos] && buf[pos] != ':' && buf[pos] != '\n') pos++;
        if (buf[pos] != ':') goto skip;
        flen = pos - start; if (flen > 31) flen = 31;
        for (int j = 0; j < flen; j++) u->username[j] = buf[start + j];
        u->username[flen] = '\0';
        pos++;

        /* Field 2: password hash */
        start = pos;
        while (buf[pos] && buf[pos] != ':' && buf[pos] != '\n') pos++;
        if (buf[pos] != ':') goto skip;
        flen = pos - start; if (flen > 128) flen = 128;
        for (int j = 0; j < flen; j++) u->passhash[j] = buf[start + j];
        u->passhash[flen] = '\0';
        pos++;

        /* Field 3: uid */
        u->uid = (uint32_t)parse_uint_str(buf, &pos);
        if (buf[pos] == ':') pos++; else goto skip;

        /* Field 4: gid */
        u->gid = (uint32_t)parse_uint_str(buf, &pos);

        /* Optional fields 5-7 */
        if (buf[pos] == ':') {
            pos++;
            /* Field 5: gecos */
            start = pos;
            while (buf[pos] && buf[pos] != ':' && buf[pos] != '\n') pos++;
            flen = pos - start; if (flen > 63) flen = 63;
            for (int j = 0; j < flen; j++) u->gecos[j] = buf[start + j];
            u->gecos[flen] = '\0';

            if (buf[pos] == ':') {
                pos++;
                /* Field 6: home */
                start = pos;
                while (buf[pos] && buf[pos] != ':' && buf[pos] != '\n') pos++;
                flen = pos - start; if (flen > 63) flen = 63;
                for (int j = 0; j < flen; j++) u->home[j] = buf[start + j];
                u->home[flen] = '\0';

                if (buf[pos] == ':') {
                    pos++;
                    /* Field 7: shell */
                    start = pos;
                    while (buf[pos] && buf[pos] != ':' && buf[pos] != '\n') pos++;
                    flen = pos - start; if (flen > 63) flen = 63;
                    for (int j = 0; j < flen; j++) u->shell[j] = buf[start + j];
                    u->shell[flen] = '\0';
                }
            }
        }

        /* Defaults */
        if (u->home[0] == '\0') {
            if (u->uid == 0) str_cpy(u->home, "/root");
            else {
                str_cpy(u->home, "/home/");
                int hi = 6;
                for (int j = 0; u->username[j] && hi < 63; j++)
                    u->home[hi++] = u->username[j];
                u->home[hi] = '\0';
            }
        }
        if (u->shell[0] == '\0') str_cpy(u->shell, "/bin/sh");

        u->active = 1;
        u->is_root = (u->uid == 0) ? 1 : 0;
        num_users++;

skip:
        while (buf[pos] && buf[pos] != '\n') pos++;
        if (buf[pos] == '\n') pos++;
    }

    AIOS_LOG_INFO_V("loaded users: ", num_users);
    return num_users;
}

/* ═══════════════════════════════════════════════════════
 *  Initialization + auto-login
 * ═══════════════════════════════════════════════════════ */
void aios_auth_init(void) {
    mem_zero(users, sizeof(users));
    mem_zero(sessions, sizeof(sessions));
    init_default_users();
    AIOS_LOG_INFO_V("default users: ", num_users);
}

uint32_t aios_auth_login_root(void) {
    /* Create root session directly (no password check) */
    for (int i = 0; i < AIOS_AUTH_MAX_SESSIONS; i++) {
        if (!sessions[i].active) {
            aios_session_t *s = &sessions[i];
            s->active = 1;
            s->token = next_token++;
            s->uid = 0;
            s->gid = 0;
            s->is_root = 1;
            s->ngroups = 0;
            str_cpy(s->username, "root");
            AIOS_LOG_INFO("root session created");
            return s->token;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════
 *  IPC thread
 * ═══════════════════════════════════════════════════════ */
void aios_auth_thread_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg0;
    (void)arg1; (void)ipc_buf;

    AIOS_LOG_INFO("auth server ready");

    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);

        switch (label) {
        case AIOS_AUTH_LOGIN:      handle_login();      break;
        case AIOS_AUTH_LOGOUT:     handle_logout();     break;
        case AIOS_AUTH_WHOAMI:     handle_whoami();     break;
        case AIOS_AUTH_CHECK_FILE: handle_check_file(); break;
        case AIOS_AUTH_CHECK_KILL: handle_check_kill(); break;
        case AIOS_AUTH_CHECK_PRIV: handle_check_priv(); break;
        case AIOS_AUTH_USERADD:    handle_useradd();    break;
        case AIOS_AUTH_PASSWD:     handle_passwd();     break;
        case AIOS_AUTH_SU:         handle_su();         break;
        case AIOS_AUTH_GROUPS:     handle_groups();     break;
        case AIOS_AUTH_USERMOD:    handle_usermod();    break;
        case AIOS_AUTH_GET_USER:   handle_get_user();   break;
        default:
            seL4_SetMR(0, AIOS_AUTH_ERR_DENIED);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
    }
}

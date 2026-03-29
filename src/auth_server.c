#include <stdint.h>
#include <microkit.h>
/*
 * auth_server.c — Authentication & Authorization PD
 *
 * Isolated protection domain for user security.
 * Owns all credential state; no other PD can read passwd data.
 *
 * Protocol (PPC from orchestrator):
 *   MR0 = command, result returned in MR0
 *   Shared memory auth_io layout:
 *     0x000  CMD       (u32)
 *     0x004  STATUS    (u32)  — 0=ok, -1=error
 *     0x008  UID       (u32)
 *     0x00C  GID       (u32)
 *     0x010  SESSION   (u32)  — session token
 *     0x040  USERNAME  (64 bytes, NUL-terminated)
 *     0x080  PASSWORD  (64 bytes, NUL-terminated — hash, not plaintext)
 *     0x0C0  PATH      (64 bytes, for ACL checks)
 *     0x100  MODE      (u32)  — requested operation (read/write/exec)
 */


/* ── Memory region ─────────────────────────────────── */
uintptr_t auth_io;
uintptr_t auth_store;  /* 64 KiB private credential store — no other PD can access */

/* ── IPC layout ────────────────────────────────────── */
#define AUTH_CMD        0x000
#define AUTH_STATUS     0x004
#define AUTH_UID        0x008
#define AUTH_GID        0x00C
#define AUTH_SESSION    0x010
#define AUTH_USERNAME   0x040
#define AUTH_PASSWORD   0x080
#define AUTH_PATH       0x0C0
#define AUTH_MODE       0x100

#define AUTH_DATA       0x200
#define AUTH_DATA_MAX   3584
/* ── Commands ──────────────────────────────────────── */
#define AUTH_CMD_LOGIN      1
#define AUTH_CMD_CHECK_FILE 2
#define AUTH_CMD_CHECK_KILL 3
#define AUTH_CMD_CHECK_PRIV 4
#define AUTH_CMD_LOGOUT     5
#define AUTH_CMD_PASSWD     10
#define AUTH_CMD_WHOAMI     7
#define AUTH_CMD_USERADD    8
#define AUTH_CMD_LOAD_PASSWD 9
#define AUTH_CMD_SU         11

/* ── Access modes ──────────────────────────────────── */
#define ACCESS_READ     0x04
#define ACCESS_WRITE    0x02
#define ACCESS_EXEC     0x01

/* ── Helpers ───────────────────────────────────────── */
#define RD32(base, off)      (*(volatile uint32_t *)((base) + (off)))
#define WR32(base, off, val) (*(volatile uint32_t *)((base) + (off)) = (val))

static int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static void my_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++));
}

static void my_memset(void *p, int v, unsigned long n) {
    unsigned char *b = (unsigned char *)p;
    for (unsigned long i = 0; i < n; i++) b[i] = (unsigned char)v;
}

/* ═══════════════════════════════════════════════════════
 *  SHA-256 (minimal, self-contained)
 * ═══════════════════════════════════════════════════════ */
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define RR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x) (RR(x,2)^RR(x,13)^RR(x,22))
#define EP1(x) (RR(x,6)^RR(x,11)^RR(x,25))
#define SIG0(x) (RR(x,7)^RR(x,18)^((x)>>3))
#define SIG1(x) (RR(x,17)^RR(x,19)^((x)>>10))

static void sha256(const uint8_t *data, uint32_t len, uint8_t hash[32]) {
    uint32_t h0=0x6a09e667, h1=0xbb67ae85, h2=0x3c6ef372, h3=0xa54ff53a;
    uint32_t h4=0x510e527f, h5=0x9b05688c, h6=0x1f83d9ab, h7=0x5be0cd19;

    /* Pad message */
    uint32_t total = len + 1 + 8;
    uint32_t padded = (total + 63) & ~63u;
    uint8_t buf[256]; /* max 192 bytes for passwords up to 120 chars */
    if (padded > sizeof(buf)) { my_memset(hash, 0, 32); return; }
    my_memset(buf, 0, padded);
    for (uint32_t i = 0; i < len; i++) buf[i] = data[i];
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[padded - 1 - i] = (uint8_t)(bits >> (i * 8));

    /* Process blocks */
    for (uint32_t blk = 0; blk < padded; blk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)buf[blk+i*4]<<24)|((uint32_t)buf[blk+i*4+1]<<16)|
                    ((uint32_t)buf[blk+i*4+2]<<8)|buf[blk+i*4+3];
        for (int i = 16; i < 64; i++)
            w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];
        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4,f=h5,g=h6,h=h7;
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h + EP1(e) + CH(e,f,g) + K256[i] + w[i];
            uint32_t t2 = EP0(a) + MAJ(a,b,c);
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e; h5+=f; h6+=g; h7+=h;
    }

    uint32_t hh[8] = {h0,h1,h2,h3,h4,h5,h6,h7};
    for (int i = 0; i < 8; i++) {
        hash[i*4]   = (uint8_t)(hh[i]>>24);
        hash[i*4+1] = (uint8_t)(hh[i]>>16);
        hash[i*4+2] = (uint8_t)(hh[i]>>8);
        hash[i*4+3] = (uint8_t)(hh[i]);
    }
}

/* Convert hash to hex string */
static void hash_to_hex(const uint8_t hash[32], char hex[65]) {
    const char *digits = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i*2]   = digits[hash[i] >> 4];
        hex[i*2+1] = digits[hash[i] & 0x0f];
    }
    hex[64] = '\0';
}

/* ═══════════════════════════════════════════════════════
 *  User database (in-memory, loaded at boot)
 * ═══════════════════════════════════════════════════════ */
#define MAX_USERS 16

typedef struct {
    int    active;
    char   username[32];
    char   passhash[65];  /* SHA-256 hex */
    uint32_t uid;
    uint32_t gid;
    int    is_root;
} user_entry_t;

static user_entry_t users[MAX_USERS];
static int          num_users = 0;

/* ── Sessions ──────────────────────────────────────── */
#define MAX_SESSIONS 4

typedef struct {
    int      active;
    uint32_t token;
    uint32_t uid;
    uint32_t gid;
    int      is_root;
    char     username[32];
} session_t;

static session_t sessions[MAX_SESSIONS];
static uint32_t  next_token = 0x1001;

/* ── Default users (compiled in; can be overridden from disk) ── */
static void init_default_users(void) {
    /* root:root */
    users[0].active = 1;
    my_strcpy(users[0].username, "root");
    /* SHA-256("root") = 4813494d137e1631bba301d5acab6e7bb7aa74ce1185d456565ef51d737677b2 */
    my_strcpy(users[0].passhash,
        "4813494d137e1631bba301d5acab6e7bb7aa74ce1185d456565ef51d737677b2");
    users[0].uid = 0;
    users[0].gid = 0;
    users[0].is_root = 1;

    /* user:user */
    users[1].active = 1;
    my_strcpy(users[1].username, "user");
    /* SHA-256("user") = 04f8996da763b7a969b1028ee3007569eaf3a635486ddab211d512c85b9df8fb */
    my_strcpy(users[1].passhash,
        "04f8996da763b7a969b1028ee3007569eaf3a635486ddab211d512c85b9df8fb");
    users[1].uid = 1000;
    users[1].gid = 1000;
    users[1].is_root = 0;

    num_users = 2;
}

/* ═══════════════════════════════════════════════════════
 *  Command handlers
 * ═══════════════════════════════════════════════════════ */

/* AUTH_CMD_LOGIN: username + password in auth_io */
void handle_login(void) {
    volatile char *uname = (volatile char *)(auth_io + AUTH_USERNAME);
    volatile char *passwd = (volatile char *)(auth_io + AUTH_PASSWORD);

    /* Copy to local buffers */
    char u[32], p[64];
    int i = 0;
    while (uname[i] && i < 31) { u[i] = uname[i]; i++; }
    u[i] = '\0';
    i = 0;
    while (passwd[i] && i < 63) { p[i] = passwd[i]; i++; }
    p[i] = '\0';

    /* Hash the password */
    uint8_t hash[32];
    sha256((const uint8_t *)p, (uint32_t)my_strlen(p), hash);
    char hex[65];
    hash_to_hex(hash, hex);

    /* Clear password from shared memory immediately */
    for (int j = 0; j < 64; j++) passwd[j] = 0;

    /* Find user */
    for (int ui = 0; ui < num_users; ui++) {
        if (!users[ui].active) continue;
        if (my_strcmp(users[ui].username, u) == 0 &&
            my_strcmp(users[ui].passhash, hex) == 0) {
            /* Match — create session */
            for (int si = 0; si < MAX_SESSIONS; si++) {
                if (!sessions[si].active) {
                    sessions[si].active = 1;
                    sessions[si].token = next_token++;
                    sessions[si].uid = users[ui].uid;
                    sessions[si].gid = users[ui].gid;
                    sessions[si].is_root = users[ui].is_root;
                    my_strcpy(sessions[si].username, users[ui].username);
                    WR32(auth_io, AUTH_STATUS, 0);
                    WR32(auth_io, AUTH_UID, users[ui].uid);
                    WR32(auth_io, AUTH_GID, users[ui].gid);
                    WR32(auth_io, AUTH_SESSION, sessions[si].token);
                    return;
                }
            }
            /* No free session slots */
            WR32(auth_io, AUTH_STATUS, (uint32_t)-2);
            return;
        }
    }
    /* No match */
    WR32(auth_io, AUTH_STATUS, (uint32_t)-1);
}

/* AUTH_CMD_LOGOUT: invalidate session */
void handle_logout(void) {
    uint32_t token = RD32(auth_io, AUTH_SESSION);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && sessions[i].token == token) {
            sessions[i].active = 0;
            WR32(auth_io, AUTH_STATUS, 0);
            return;
        }
    }
    WR32(auth_io, AUTH_STATUS, (uint32_t)-1);
}

/* AUTH_CMD_CHECK_FILE: verify file access permission */
void handle_check_file(void) {
    uint32_t token = RD32(auth_io, AUTH_SESSION);
    uint32_t mode  = RD32(auth_io, AUTH_MODE);
    volatile char *path = (volatile char *)(auth_io + AUTH_PATH);

    /* Find session */
    session_t *sess = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && sessions[i].token == token) {
            sess = &sessions[i];
            break;
        }
    }
    if (!sess) { WR32(auth_io, AUTH_STATUS, (uint32_t)-1); return; }

    /* Root can do anything */
    if (sess->is_root) { WR32(auth_io, AUTH_STATUS, 0); return; }

    /* Protect /etc/ from non-root */
    char p[64]; int pi = 0;
    while (path[pi] && pi < 63) { p[pi] = path[pi]; pi++; }
    p[pi] = '\0';

    /* Check /etc/ prefix — non-root cannot write */
    if (p[0]=='/' && p[1]=='e' && p[2]=='t' && p[3]=='c' && p[4]=='/') {
        if (mode & ACCESS_WRITE) {
            WR32(auth_io, AUTH_STATUS, (uint32_t)-1);
            return;
        }
    }

    /* Default: allow (ext2 permission enforcement is Phase 3) */
    WR32(auth_io, AUTH_STATUS, 0);
}

/* AUTH_CMD_CHECK_KILL: can caller kill target? */
void handle_check_kill(void) {
    uint32_t token      = RD32(auth_io, AUTH_SESSION);
    uint32_t target_uid = RD32(auth_io, AUTH_UID);

    session_t *sess = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && sessions[i].token == token) {
            sess = &sessions[i];
            break;
        }
    }
    if (!sess) { WR32(auth_io, AUTH_STATUS, (uint32_t)-1); return; }

    /* Root can kill anything; users can only kill their own processes */
    if (sess->is_root || sess->uid == target_uid) {
        WR32(auth_io, AUTH_STATUS, 0);
    } else {
        WR32(auth_io, AUTH_STATUS, (uint32_t)-1);
    }
}

/* AUTH_CMD_CHECK_PRIV: is caller root? */
void handle_check_priv(void) {
    uint32_t token = RD32(auth_io, AUTH_SESSION);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && sessions[i].token == token) {
            if (sessions[i].is_root) {
                WR32(auth_io, AUTH_STATUS, 0);
            } else {
                WR32(auth_io, AUTH_STATUS, (uint32_t)-1);
            }
            return;
        }
    }
    WR32(auth_io, AUTH_STATUS, (uint32_t)-1);
}

/* AUTH_CMD_WHOAMI: return username for session */
void handle_whoami(void) {
    uint32_t token = RD32(auth_io, AUTH_SESSION);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && sessions[i].token == token) {
            volatile char *dst = (volatile char *)(auth_io + AUTH_USERNAME);
            int j = 0;
            while (sessions[i].username[j] && j < 31) {
                dst[j] = sessions[i].username[j]; j++;
            }
            dst[j] = '\0';
            WR32(auth_io, AUTH_UID, sessions[i].uid);
            WR32(auth_io, AUTH_GID, sessions[i].gid);
            WR32(auth_io, AUTH_STATUS, 0);
            return;
        }
    }
    WR32(auth_io, AUTH_STATUS, (uint32_t)-1);
}

/* AUTH_CMD_USERADD: root only — add user */
void handle_useradd(void) {
    uint32_t token = RD32(auth_io, AUTH_SESSION);

    /* Verify caller is root */
    session_t *sess = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && sessions[i].token == token) {
            sess = &sessions[i]; break;
        }
    }
    if (!sess || !sess->is_root) {
        WR32(auth_io, AUTH_STATUS, (uint32_t)-1);
        return;
    }
    if (num_users >= MAX_USERS) {
        WR32(auth_io, AUTH_STATUS, (uint32_t)-2);
        return;
    }

    volatile char *uname  = (volatile char *)(auth_io + AUTH_USERNAME);
    volatile char *passwd = (volatile char *)(auth_io + AUTH_PASSWORD);
    uint32_t new_uid = RD32(auth_io, AUTH_UID);
    uint32_t new_gid = RD32(auth_io, AUTH_GID);

    char u[32], p[64];
    int i = 0;
    while (uname[i] && i < 31) { u[i] = uname[i]; i++; }
    u[i] = '\0';
    i = 0;
    while (passwd[i] && i < 63) { p[i] = passwd[i]; i++; }
    p[i] = '\0';

    /* Hash password */
    uint8_t hash[32];
    sha256((const uint8_t *)p, (uint32_t)my_strlen(p), hash);
    char hex[65];
    hash_to_hex(hash, hex);

    /* Clear password from shared memory */
    for (int j = 0; j < 64; j++) passwd[j] = 0;

    /* Check for duplicate username */
    for (int ui = 0; ui < num_users; ui++) {
        if (users[ui].active && my_strcmp(users[ui].username, u) == 0) {
            WR32(auth_io, AUTH_STATUS, (uint32_t)-3);
            return;
        }
    }

    /* Add user */
    user_entry_t *nu = &users[num_users++];
    nu->active = 1;
    my_strcpy(nu->username, u);
    my_strcpy(nu->passhash, hex);
    nu->uid = new_uid;
    nu->gid = new_gid;
    nu->is_root = (new_uid == 0) ? 1 : 0;
    WR32(auth_io, AUTH_STATUS, 0);
}


/* AUTH_CMD_LOAD_PASSWD: parse /etc/passwd from auth_io+AUTH_DATA
 * Format: "username:sha256hash:uid:gid\n" per line
 */
static int parse_uint(const char *s, int *pos) {
    int val = 0;
    while (s[*pos] >= '0' && s[*pos] <= '9') {
        val = val * 10 + (s[*pos] - '0');
        (*pos)++;
    }
    return val;
}

void handle_load_passwd(void) {
    volatile char *data = (volatile char *)(auth_io + AUTH_DATA);
    char buf[3584];
    int i = 0;
    while (data[i] && i < 3583) { buf[i] = data[i]; i++; }
    buf[i] = '\0';

    /* Reset user database */
    my_memset(users, 0, sizeof(users));
    num_users = 0;

    /* Parse line by line */
    int pos = 0;
    while (buf[pos] && num_users < MAX_USERS) {
        /* username */
        int start = pos;
        while (buf[pos] && buf[pos] != ':') pos++;
        if (buf[pos] != ':') break;
        int ulen = pos - start;
        if (ulen > 31) ulen = 31;

        user_entry_t *u = &users[num_users];
        for (int j = 0; j < ulen; j++) u->username[j] = buf[start + j];
        u->username[ulen] = '\0';
        pos++; /* skip : */

        /* password hash */
        start = pos;
        while (buf[pos] && buf[pos] != ':') pos++;
        if (buf[pos] != ':') break;
        int hlen = pos - start;
        if (hlen > 64) hlen = 64;
        for (int j = 0; j < hlen; j++) u->passhash[j] = buf[start + j];
        u->passhash[hlen] = '\0';
        pos++; /* skip : */

        /* uid */
        u->uid = (uint32_t)parse_uint(buf, &pos);
        if (buf[pos] == ':') pos++;

        /* gid */
        u->gid = (uint32_t)parse_uint(buf, &pos);

        u->active = 1;
        u->is_root = (u->uid == 0) ? 1 : 0;
        num_users++;

        /* Skip to next line */
        while (buf[pos] && buf[pos] != '\n') pos++;
        if (buf[pos] == '\n') pos++;
    }

    /* Clear data from shared memory */
    for (int j = 0; j < 3584; j++) data[j] = 0;

    microkit_dbg_puts("AUTH: loaded ");
    char nb[4]; int n = num_users; int ni = 0;
    if (n == 0) { nb[ni++] = '0'; }
    else { while (n > 0) { nb[ni++] = '0' + (n % 10); n /= 10; } }
    for (int j = ni - 1; j >= 0; j--) microkit_dbg_putc(nb[j]);
    microkit_dbg_puts(" users from /etc/passwd\n");

    WR32(auth_io, AUTH_STATUS, 0);
}

/* AUTH_CMD_PASSWD: change password for a user
 * AUTH_USERNAME = target user, AUTH_PASSWORD = new password
 * Caller must be root OR target user (old password verified via session)
 */
void handle_passwd_change(void) {
    uint32_t token = RD32(auth_io, AUTH_SESSION);
    volatile char *uname  = (volatile char *)(auth_io + AUTH_USERNAME);
    volatile char *passwd = (volatile char *)(auth_io + AUTH_PASSWORD);

    /* Find session */
    session_t *sess = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && sessions[i].token == token) {
            sess = &sessions[i]; break;
        }
    }
    if (!sess) { WR32(auth_io, AUTH_STATUS, (uint32_t)-1); return; }

    char u[32], p[64];
    int i = 0;
    while (uname[i] && i < 31) { u[i] = uname[i]; i++; }
    u[i] = '\0';
    i = 0;
    while (passwd[i] && i < 63) { p[i] = passwd[i]; i++; }
    p[i] = '\0';

    /* Only root can change other users' passwords */
    if (!sess->is_root && my_strcmp(sess->username, u) != 0) {
        for (int j = 0; j < 64; j++) passwd[j] = 0;
        WR32(auth_io, AUTH_STATUS, (uint32_t)-1);
        return;
    }

    /* Find target user */
    for (int ui = 0; ui < num_users; ui++) {
        if (users[ui].active && my_strcmp(users[ui].username, u) == 0) {
            /* Hash new password */
            uint8_t hash[32];
            sha256((const uint8_t *)p, (uint32_t)my_strlen(p), hash);
            char hex[65];
            hash_to_hex(hash, hex);
            my_strcpy(users[ui].passhash, hex);
            /* Clear password from shared memory */
            for (int j = 0; j < 64; j++) passwd[j] = 0;
            WR32(auth_io, AUTH_STATUS, 0);
            return;
        }
    }
    for (int j = 0; j < 64; j++) passwd[j] = 0;
    WR32(auth_io, AUTH_STATUS, (uint32_t)-2); /* user not found */
}


/* AUTH_CMD_SU: switch user — verify password, update session */
void handle_su(void) {
    volatile char *uname  = (volatile char *)(auth_io + AUTH_USERNAME);
    volatile char *passwd = (volatile char *)(auth_io + AUTH_PASSWORD);
    uint32_t token = RD32(auth_io, AUTH_SESSION);

    char u[32], p[64];
    int i = 0;
    while (uname[i] && i < 31) { u[i] = uname[i]; i++; }
    u[i] = '\0';
    i = 0;
    while (passwd[i] && i < 63) { p[i] = passwd[i]; i++; }
    p[i] = '\0';

    /* Find current session */
    session_t *sess = 0;
    for (int si = 0; si < MAX_SESSIONS; si++) {
        if (sessions[si].active && sessions[si].token == token) {
            sess = &sessions[si]; break;
        }
    }
    if (!sess) {
        for (int j = 0; j < 64; j++) passwd[j] = 0;
        WR32(auth_io, AUTH_STATUS, (uint32_t)-1);
        return;
    }

    /* Root can su without password */
    if (sess->is_root && p[0] == '\0') {
        /* Find target user */
        for (int ui = 0; ui < num_users; ui++) {
            if (users[ui].active && my_strcmp(users[ui].username, u) == 0) {
                sess->uid = users[ui].uid;
                sess->gid = users[ui].gid;
                sess->is_root = users[ui].is_root;
                my_strcpy(sess->username, users[ui].username);
                WR32(auth_io, AUTH_UID, users[ui].uid);
                WR32(auth_io, AUTH_GID, users[ui].gid);
                WR32(auth_io, AUTH_STATUS, 0);
                return;
            }
        }
        WR32(auth_io, AUTH_STATUS, (uint32_t)-2);
        return;
    }

    /* Non-root must provide password */
    uint8_t hash[32];
    sha256((const uint8_t *)p, (uint32_t)my_strlen(p), hash);
    char hex[65];
    hash_to_hex(hash, hex);
    for (int j = 0; j < 64; j++) passwd[j] = 0;

    for (int ui = 0; ui < num_users; ui++) {
        if (users[ui].active && my_strcmp(users[ui].username, u) == 0 &&
            my_strcmp(users[ui].passhash, hex) == 0) {
            sess->uid = users[ui].uid;
            sess->gid = users[ui].gid;
            sess->is_root = users[ui].is_root;
            my_strcpy(sess->username, users[ui].username);
            WR32(auth_io, AUTH_UID, users[ui].uid);
            WR32(auth_io, AUTH_GID, users[ui].gid);
            WR32(auth_io, AUTH_STATUS, 0);
            return;
        }
    }
    WR32(auth_io, AUTH_STATUS, (uint32_t)-1);
}

/* ═══════════════════════════════════════════════════════
 *  Microkit entry points
 * ═══════════════════════════════════════════════════════ */

void init(void) {
    my_memset(users, 0, sizeof(users));
    my_memset(sessions, 0, sizeof(sessions));
    init_default_users();
    microkit_dbg_puts("AUTH: server ready (");
    /* Print user count */
    char nb[4]; int n = num_users; int ni = 0;
    if (n == 0) { nb[ni++] = '0'; }
    else { while (n > 0) { nb[ni++] = '0' + (n % 10); n /= 10; } }
    for (int i = ni - 1; i >= 0; i--) microkit_dbg_putc(nb[i]);
    microkit_dbg_puts(" users)\n");
}

void notified(microkit_channel ch) {
    (void)ch;
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch;
    (void)msginfo;

    uint32_t cmd = RD32(auth_io, AUTH_CMD);
    switch (cmd) {
    case AUTH_CMD_LOGIN:      handle_login();      break;
    case AUTH_CMD_CHECK_FILE: handle_check_file(); break;
    case AUTH_CMD_CHECK_KILL: handle_check_kill(); break;
    case AUTH_CMD_CHECK_PRIV: handle_check_priv(); break;
    case AUTH_CMD_LOGOUT:     handle_logout();     break;
    case AUTH_CMD_WHOAMI:     handle_whoami();     break;
    case AUTH_CMD_USERADD:    handle_useradd();    break;
    case AUTH_CMD_LOAD_PASSWD: handle_load_passwd(); break;
    case AUTH_CMD_PASSWD:     handle_passwd_change(); break;
    case AUTH_CMD_SU:         handle_su();          break;
    default:
        WR32(auth_io, AUTH_STATUS, (uint32_t)-1);
        break;
    }
    return microkit_msginfo_new(0, 0);
}

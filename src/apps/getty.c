/*
 * AIOS getty -- login gate
 *
 * Displays banner, handles authentication, then fork+exec mini_shell.
 * When shell exits (logout), loops back to login prompt.
 *
 * AiosChildApp (not PosixApp) -- manual cap parsing.
 * Links aios_posix for fork/exec/waitpid.
 */
#include <stdint.h>
#include <sel4/sel4.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "aios_posix.h"

extern seL4_CPtr pipe_ep;

#define SER_PUTC  1
#define SER_GETC  2
#define AUTH_LOGIN 40
#define PIPE_SET_IDENTITY 74

static seL4_CPtr serial_ep, fs_ep, auth_ep;

/* ---- Helpers ---- */

static long parse_num(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void str_cpy(char *d, const char *s) { while ((*d++ = *s++)); }

static void ser_putc(char c) {
    seL4_SetMR(0, (seL4_Word)c);
    seL4_Call(serial_ep, seL4_MessageInfo_new(SER_PUTC, 0, 0, 1));
}
static void ser_puts(const char *s) { while (*s) ser_putc(*s++); }
static int ser_getc(void) {
    seL4_MessageInfo_t r = seL4_Call(serial_ep,
        seL4_MessageInfo_new(SER_GETC, 0, 0, 0));
    return (int)(long)seL4_GetMR(0);
}

/* ---- Password input with masking ---- */

static void read_password(char *buf, int max) {
    int len = 0;
    while (len < max - 1) {
        int c = ser_getc();
        if (c < 0) continue;
        if (c == '\r' || c == '\n') { ser_putc('\n'); break; }
        if ((c == 0x7f || c == '\b') && len > 0) {
            len--; ser_putc('\b'); ser_putc(' '); ser_putc('\b');
            continue;
        }
        if (c == 0x1b) {
            int c2 = ser_getc();
            if (c2 == '[') ser_getc();
            continue;
        }
        if (c >= 0x20 && c < 127) { buf[len++] = (char)c; ser_putc('*'); }
    }
    buf[len] = '\0';
}

/* ---- Login (AUTH_LOGIN IPC) ---- */

static int do_login(uint32_t *uid, uint32_t *gid,
                    uint32_t *token, char *username) {
    if (!auth_ep) {
        /* No auth server -- auto-login as root */
        str_cpy(username, "root");
        *uid = 0; *gid = 0; *token = 1;
        return 1;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        char password[64];
        int ulen = 0;

        ser_puts("\nAIOS login: ");
        while (ulen < 31) {
            int c = ser_getc();
            if (c < 0) continue;
            if (c == '\r' || c == '\n') { ser_putc('\n'); break; }
            if ((c == 0x7f || c == '\b') && ulen > 0) {
                ulen--; ser_putc('\b'); ser_putc(' '); ser_putc('\b');
                continue;
            }
            if (c == 0x1b) {
                int c2 = ser_getc();
                if (c2 == '[') ser_getc();
                continue;
            }
            if (c >= 0x20 && c < 127) {
                username[ulen++] = (char)c;
                ser_putc((char)c);
            }
        }
        username[ulen] = '\0';
        if (ulen == 0) continue;

        ser_puts("Password: ");
        read_password(password, 64);

        /* Pack username into MRs */
        int mr = 0;
        seL4_SetMR(mr++, (seL4_Word)ulen);
        seL4_Word w = 0;
        for (int i = 0; i < ulen; i++) {
            w |= ((seL4_Word)(uint8_t)username[i]) << ((i % 8) * 8);
            if (i % 8 == 7 || i == ulen - 1) { seL4_SetMR(mr++, w); w = 0; }
        }

        /* Pack password */
        int plen = str_len(password);
        seL4_SetMR(mr++, (seL4_Word)plen);
        w = 0;
        for (int i = 0; i < plen; i++) {
            w |= ((seL4_Word)(uint8_t)password[i]) << ((i % 8) * 8);
            if (i % 8 == 7 || i == plen - 1) { seL4_SetMR(mr++, w); w = 0; }
        }

        /* Scrub password from stack */
        for (int i = 0; i < 64; i++) password[i] = 0;

        seL4_MessageInfo_t reply = seL4_Call(auth_ep,
            seL4_MessageInfo_new(AUTH_LOGIN, 0, 0, mr));
        uint32_t status = (uint32_t)seL4_GetMR(0);
        if (status == 0) {
            *uid = (uint32_t)seL4_GetMR(1);
            *gid = (uint32_t)seL4_GetMR(2);
            *token = (uint32_t)seL4_GetMR(3);
            return 1;
        }
        ser_puts("Login incorrect\n");
    }
    return 0;
}

/* ---- MOTD display via FS_CAT IPC ---- */

static void display_motd(void) {
    if (!fs_ep) return;
    char mpath[] = "/etc/motd";
    int mpl = 9;
    seL4_SetMR(0, (seL4_Word)mpl);
    int mmr = 1;
    seL4_Word mw = 0;
    for (int i = 0; i < mpl; i++) {
        mw |= ((seL4_Word)(uint8_t)mpath[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == mpl - 1) { seL4_SetMR(mmr++, mw); mw = 0; }
    }
    seL4_MessageInfo_t mreply = seL4_Call(fs_ep,
        seL4_MessageInfo_new(11, 0, 0, mmr));
    seL4_Word mtotal = seL4_GetMR(0);
    if (mtotal > 0) {
        int mrmrs = (int)seL4_MessageInfo_get_length(mreply) - 1;
        int mgot = 0;
        for (int i = 0; i < mrmrs; i++) {
            seL4_Word rw = seL4_GetMR(i + 1);
            for (int j = 0; j < 8 && mgot < (int)mtotal; j++) {
                ser_putc((char)((rw >> (j * 8)) & 0xFF));
                mgot++;
            }
        }
    }
}

/* ---- Main: banner, login loop, fork+exec shell ---- */

int main(int argc, char *argv[]) {
    serial_ep = 0; fs_ep = 0; auth_ep = 0;
    if (argc > 0) serial_ep = (seL4_CPtr)parse_num(argv[0]);
    if (argc > 1) fs_ep     = (seL4_CPtr)parse_num(argv[1]);
    if (argc > 3) auth_ep   = (seL4_CPtr)parse_num(argv[3]);
    if (argc > 4) pipe_ep   = (seL4_CPtr)parse_num(argv[4]);

    /* Init POSIX shim (needed for fork/exec/waitpid) */
    aios_init_caps(serial_ep, fs_ep, auth_ep, pipe_ep);

    ser_puts("\n============================================\n");
    ser_puts("  AIOS 0.4.x\n");
    ser_puts("============================================\n");

    while (1) {
        uint32_t uid = 0, gid = 0, token = 0;
        char username[32];
        username[0] = '\0';

        if (!do_login(&uid, &gid, &token, username)) {
            ser_puts("Too many failed attempts.\n");
            continue;
        }

        /* Tell pipe_server our new identity so fork+exec
         * propagates uid/gid to the child shell */
        if (pipe_ep) {
            seL4_SetMR(0, (seL4_Word)uid);
            seL4_SetMR(1, (seL4_Word)gid);
            seL4_Call(pipe_ep,
                seL4_MessageInfo_new(PIPE_SET_IDENTITY, 0, 0, 2));
        }

        display_motd();
        ser_puts("Welcome, "); ser_puts(username); ser_puts("\n\n");

        /* Fork+exec mini_shell, wait for it to exit */
        pid_t pid = fork();
        if (pid < 0) {
            ser_puts("getty: fork failed\n");
            continue;
        }
        if (pid == 0) {
            char *sh_argv[] = {"mini_shell", (void *)0};
            execv("/bin/mini_shell", sh_argv);
            _exit(127);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        /* Shell exited -- loop back to login */
        ser_puts("\n");
    }
    return 0;
}

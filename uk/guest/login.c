/*
 * login.c -- the AIOS login program (system layer, increment 1). An AIOS-ABI program (libaios), run
 * by /sbin/init on the console: it prints a banner, prompts for a username + password (the password
 * read with terminal ECHO off via the termios layer), authenticates against /etc/shadow, and on
 * success becomes the user's login shell (exec with argv[0] = "-sh" so dash sources /etc/profile).
 *
 * INCREMENT 2: login SWITCHES USER -- on success it setgid/setuid's to the authenticated user
 * (privileged drop from uid 0), so whoami/id/$LOGNAME reflect them for the whole session. Passwords in
 * /etc/shadow are verified with crypt() (SHA-512 "$6$" hashes, byte-identical to host glibc); a non-'$'
 * secret is still accepted as legacy plaintext (transitional). Everything here is the AIOS ABI -- never
 * a host call.
 */
#include "aios_version.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <pwd.h>
#include <libgen.h>

static void chomp(char *s) { size_t n = strlen(s); while (n && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0'; }

/* Read one line from stdin UNBUFFERED (one byte at a time) -- crucial: buffered stdio (fgets) would
 * over-read past the line into the FILE buffer, so the shell we exec would lose the rest of stdin
 * (its first commands). Returns the length (>= 0), or -1 at EOF with nothing read. Strips the newline. */
static int read_line(char *buf, int size) {
    int n = 0, got = 0; char c;
    while (n < size - 1) {
        long r = read(0, &c, 1);
        if (r <= 0) { if (!got) return -1; break; }
        got = 1;
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

/* Check `pw` against the secret in /etc/shadow field 2 for `user`. A secret beginning with '$' is a
 * crypt() hash (the $6$ SHA-512 scheme): recompute crypt(pw, stored) and compare to the stored hash
 * (constant-form, the standard verify). A non-'$' secret is treated as legacy PLAINTEXT (transitional);
 * an empty secret denies (no blank logins). */
static int password_ok(const char *user, const char *pw) {
    FILE *f = fopen("/etc/shadow", "r");
    if (!f) return 0;
    char line[256]; int ok = 0;
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        char *c1 = strchr(line, ':'); if (!c1) continue; *c1 = '\0';
        if (strcmp(line, user) != 0) continue;
        char *secret = c1 + 1; char *c2 = strchr(secret, ':'); if (c2) *c2 = '\0';
        if (secret[0] == '$') {                          /* a crypt() hash -> recompute + compare */
            char *h = crypt(pw, secret);
            ok = (h && strcmp(h, secret) == 0);
        } else {                                         /* legacy plaintext (deny an empty secret) */
            ok = (secret[0] != '\0' && strcmp(secret, pw) == 0);
        }
        break;
    }
    fclose(f);
    return ok;
}

static void show_file(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char b[256]; while (fgets(b, sizeof b, f)) fputs(b, stdout);
    fclose(f);
}

int main(void) {
    char user[64], pass[128];
    for (;;) {
        printf("\nAIOS %s -- userspace kernel\n", AIOS_VERSION_STR);
        printf("login: "); fflush(stdout);
        if (read_line(user, sizeof user) < 0) return 0;   /* EOF -> exit; init respawns us */
        if (!user[0]) continue;

        /* Read the password with terminal ECHO off (when on a tty; on a pipe it is read plain). */
        struct termios t, saved; int tty = (tcgetattr(0, &t) == 0);
        if (tty) { saved = t; t.c_lflag &= ~(unsigned)ECHO; tcsetattr(0, TCSANOW, &t); }
        printf("Password: "); fflush(stdout);
        if (read_line(pass, sizeof pass) < 0) { if (tty) tcsetattr(0, TCSANOW, &saved); return 0; }
        if (tty) { tcsetattr(0, TCSANOW, &saved); printf("\n"); }

        struct passwd *u = getpwnam(user);
        if (u && password_ok(user, pass)) {
            /* Become the authenticated user. login runs as init's child (AIOS root, uid 0), so it is
             * privileged: setgid BEFORE setuid (once uid drops from 0 we lose the right to setgid).
             * After this the whole session -- motd, the shell, every command -- runs as the user, so
             * whoami/id/$LOGNAME reflect them. (AIOS identity is the kernel's model; the host still
             * owns actual file ownership, so this is identity, not yet uid-based file access control.) */
            if (setgid(u->pw_gid) != 0 || setuid(u->pw_uid) != 0) {
                printf("login: cannot switch to uid %u\n", u->pw_uid);
                return 1;   /* never expected (login is privileged); init respawns us */
            }
            show_file("/etc/motd");
            const char *sh   = (u->pw_shell && u->pw_shell[0]) ? u->pw_shell : "/bin/sh";
            const char *home = (u->pw_dir   && u->pw_dir[0])   ? u->pw_dir   : "/";
            char henv[160], uenv[96], lenv[96], senv[160];
            static char penv[] = "PATH=/bin:/sbin";
            snprintf(henv, sizeof henv, "HOME=%s", home);
            snprintf(uenv, sizeof uenv, "USER=%s", user);
            snprintf(lenv, sizeof lenv, "LOGNAME=%s", user);
            snprintf(senv, sizeof senv, "SHELL=%s", sh);
            char *envp[] = { henv, uenv, lenv, senv, penv, NULL };
            /* login shell: argv[0] = "-<base>" so dash treats it as a login shell (sources /etc/profile) */
            char shcopy[160], argv0[64];
            snprintf(shcopy, sizeof shcopy, "%s", sh);
            snprintf(argv0, sizeof argv0, "-%s", basename(shcopy));
            char *argv[] = { argv0, NULL };
            chdir(home);
            fflush(stdout);   /* flush the motd + prompts before the shell takes over */
            execve(sh, argv, envp);
            perror("login: exec shell");
            return 1;
        }
        printf("Login incorrect\n");
    }
}

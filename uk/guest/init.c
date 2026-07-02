/*
 * init.c -- the AIOS system init (system layer). The FIRST guest the AIOS kernel launches (the
 * appliance's /sbin/init): it owns the console and runs the system from a config file, /etc/inittab.
 *
 * INCREMENT 2: init is now CONFIG-DRIVEN. It parses /etc/inittab ("id:runlevels:action:process";
 * runlevels are ignored) and supports these actions:
 *   sysinit / wait  -- run the process once at boot, BLOCK until it exits (setup steps);
 *   once            -- run the process once at boot, do NOT wait;
 *   respawn         -- keep the process running: fork it, and restart it whenever it exits (a getty).
 * The console login is a `respawn` entry. If /etc/inittab is missing or has no respawn entry, init
 * falls back to respawning /bin/login (increment-1 behaviour), so the system always comes up.
 *
 * The AIOS kernel reaps orphaned guests itself; init manages only the children it starts. A clean
 * shutdown is the kernel's job (a root `poweroff`/`reboot` exits aios-uk), so init just supervises.
 * An AIOS-ABI program (libaios). (libaios sleep() is a no-op, so anti-thrash uses a clock nap.)
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>

#define MAX_ENT  16
#define MAX_ARGS 16

struct entry {
    int    respawn;              /* 1 = keep alive (getty); 0 = run once */
    int    waitfor;             /* run-once entries: block until it exits (sysinit/wait) */
    pid_t  pid;                  /* respawn entries: the current child pid (0 = not running) */
    char  *argv[MAX_ARGS];
    char   buf[256];             /* backing storage for the split argv */
};
static struct entry ents[MAX_ENT];
static int nent;

/* A real short pause (libaios sleep() is a no-op): busy-wait on the monotonic clock -- keeps a
 * respawn that exits immediately (e.g. a broken getty) from spinning tight. */
static void nap_ms(long ms) {
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &b);
        long d = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
        if (d >= ms) return;
    }
}

static void chomp(char *s) { size_t n = strlen(s); while (n && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0'; }

/* Split `proc` into e->argv on whitespace, storing into e->buf. */
static void split_argv(struct entry *e, const char *proc) {
    size_t i = 0; for (; proc[i] && i < sizeof e->buf - 1; i++) e->buf[i] = proc[i];
    e->buf[i] = '\0';
    int n = 0; char *s = e->buf;
    while (*s && n < MAX_ARGS - 1) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        e->argv[n++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = '\0';
    }
    e->argv[n] = 0;
}

/* Parse /etc/inittab into ents[]. Returns the number of entries (0 if the file is absent/empty). */
static int parse_inittab(void) {
    FILE *f = fopen("/etc/inittab", "r");
    if (!f) return 0;
    char line[256];
    while (nent < MAX_ENT && fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#'); if (hash) *hash = '\0';   /* strip comments */
        chomp(line);
        if (!line[0]) continue;
        char *c1 = strchr(line, ':'); if (!c1) continue; *c1++ = '\0';   /* id (ignored)       */
        char *c2 = strchr(c1, ':');   if (!c2) continue; *c2++ = '\0';   /* runlevels (ignored) */
        char *c3 = strchr(c2, ':');   if (!c3) continue; *c3++ = '\0';   /* action ; c3=process */
        const char *action = c2, *proc = c3;
        struct entry *e = &ents[nent];
        if      (!strcmp(action, "respawn")) { e->respawn = 1; e->waitfor = 0; }
        else if (!strcmp(action, "sysinit") || !strcmp(action, "wait")) { e->respawn = 0; e->waitfor = 1; }
        else if (!strcmp(action, "once") || !strcmp(action, "boot"))     { e->respawn = 0; e->waitfor = 0; }
        else continue;                                            /* unsupported action -> ignore */
        split_argv(e, proc);
        if (!e->argv[0]) continue;
        nent++;
    }
    fclose(f);
    return nent;
}

static pid_t spawn(struct entry *e) {
    pid_t pid = fork();
    if (pid == 0) { execv(e->argv[0], e->argv); fputs("[init] cannot exec ", stderr);
                    fputs(e->argv[0], stderr); fputs("\n", stderr); _exit(127); }
    return pid;
}

int main(void) {
    fputs("\n[init] AIOS system init -- reading /etc/inittab\n", stderr);

    if (parse_inittab() == 0) {                       /* no inittab -> the increment-1 default getty */
        struct entry *e = &ents[nent++];
        e->respawn = 1; split_argv(e, "/bin/login");
    }

    /* run-once entries first (sysinit/wait block; once does not). */
    int have_respawn = 0;
    for (int i = 0; i < nent; i++) {
        if (ents[i].respawn) { have_respawn = 1; continue; }
        pid_t pid = spawn(&ents[i]);
        if (pid > 0 && ents[i].waitfor) { int st; while (waitpid(pid, &st, 0) < 0) { } }
    }
    if (!have_respawn) {                              /* config with only run-once entries: fall back */
        struct entry *e = &ents[nent++];
        e->respawn = 1; split_argv(e, "/bin/login");
    }

    /* start every respawn entry, then supervise: on a child exit, respawn the matching entry. */
    for (int i = 0; i < nent; i++) if (ents[i].respawn) ents[i].pid = spawn(&ents[i]);
    for (;;) {
        int st; pid_t dead = waitpid(-1, &st, 0);
        if (dead < 0) { nap_ms(200); continue; }      /* no children (shouldn't happen) -> back off */
        for (int i = 0; i < nent; i++)
            if (ents[i].respawn && ents[i].pid == dead) {
                nap_ms(300);                           /* brief anti-thrash pause, then respawn */
                ents[i].pid = spawn(&ents[i]);
                break;
            }
    }
}

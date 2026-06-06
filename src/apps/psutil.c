/* psutil.c -- pidof / pkill / killall for AIOS, dispatched by argv[0].
 *
 *   pidof   <name>          print the pids of every process whose program
 *                           BASENAME equals <name> (space-separated, one line)
 *   killall [-SIG] <name>   send SIG (default TERM) to every EXACT basename match
 *   pkill   [-SIG] <pat>    send SIG (default TERM) to every SUBSTRING match
 *
 * Pure userspace: reads the process table from /proc/status (columns
 * "PID PRI NICE STATE UID THR NAME"; exec-ed procs show the full path the shell
 * sent, e.g. /bin/netconsole2, so we match on the basename) and signals via
 * kill(2) (aios_sys_kill -> PIPE_SIGNAL; SIGTERM/SIGKILL destroy the target).
 * Never signals itself. No root-task change, no reflash -- build with aios-cc,
 * install the one binary under all three names.
 *
 * Build:  ./scripts/aios-cc src/apps/psutil.c -o build-04/sbase/pidof
 *         cp build-04/sbase/pidof build-04/sbase/pkill
 *         cp build-04/sbase/pidof build-04/sbase/killall
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#define MAXPROC 128

/* basename: pointer to the char after the last '/'. */
static const char *base(const char *p)
{
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '/') b = q + 1;
    return b;
}

/* naive substring test (avoid depending on strstr). */
static int substr(const char *hay, const char *needle)
{
    if (!*needle) return 1;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *n = needle;
        while (*a && *n && *a == *n) { a++; n++; }
        if (!*n) return 1;
    }
    return 0;
}

/* Read /proc/status -> pids[]/names[]. Returns process count, or -1. */
static int read_procs(int *pids, char names[][64], int max)
{
    int fd = open("/proc/status", O_RDONLY);
    if (fd < 0) return -1;
    static char buf[8192];
    int total = 0, n;
    while (total < (int)sizeof(buf) - 1 &&
           (n = (int)read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
        total += n;
    close(fd);
    buf[total] = '\0';

    int count = 0, line_no = 0;
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line_no++ > 0 && *line && count < max) {   /* line 0 is the header */
            /* first token = pid */
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            int pid = 0, havep = 0;
            while (*s >= '0' && *s <= '9') { pid = pid * 10 + (*s - '0'); s++; havep = 1; }
            /* last whitespace-delimited token = name */
            char *e = line + strlen(line);
            while (e > line && (e[-1] == ' ' || e[-1] == '\t')) e--;
            char *b = e;
            while (b > line && b[-1] != ' ' && b[-1] != '\t') b--;
            if (havep && e > b) {
                int ni = 0;
                while (b < e && ni < 63) names[count][ni++] = *b++;
                names[count][ni] = '\0';
                pids[count++] = pid;
            }
        }
        line = nl ? nl + 1 : 0;
    }
    return count;
}

static int parse_sig(const char *a)
{
    a++;   /* skip the leading '-' */
    if (*a >= '0' && *a <= '9') {
        int v = 0;
        while (*a >= '0' && *a <= '9') v = v * 10 + (*a++ - '0');
        return v;
    }
    if (!strcmp(a, "KILL") || !strcmp(a, "SIGKILL")) return 9;
    if (!strcmp(a, "TERM") || !strcmp(a, "SIGTERM")) return 15;
    if (!strcmp(a, "INT")  || !strcmp(a, "SIGINT"))  return 2;
    if (!strcmp(a, "HUP")  || !strcmp(a, "SIGHUP"))  return 1;
    if (!strcmp(a, "QUIT") || !strcmp(a, "SIGQUIT")) return 3;
    return 15;   /* default TERM */
}

int main(int argc, char **argv)
{
    const char *prog = base(argv[0]);
    int is_pidof   = !strcmp(prog, "pidof");
    int is_pkill   = !strcmp(prog, "pkill");
    int is_killall = !strcmp(prog, "killall");
    if (!is_pidof && !is_pkill && !is_killall) is_pidof = 1;   /* default mode */
    (void)is_killall;

    int sig = 15, ai = 1;
    if (!is_pidof && ai < argc && argv[ai][0] == '-' && argv[ai][1]) {
        sig = parse_sig(argv[ai]); ai++;
    }
    if (ai >= argc) {
        printf("usage: %s %s<name>\n", prog, is_pidof ? "" : "[-SIG] ");
        return 2;
    }
    const char *target = argv[ai];
    const char *tbase = base(target);

    static int pids[MAXPROC];
    static char names[MAXPROC][64];
    int n = read_procs(pids, names, MAXPROC);
    if (n < 0) { printf("%s: cannot read /proc/status\n", prog); return 2; }

    int self = getpid();
    int matched = 0, printed = 0;
    for (int i = 0; i < n; i++) {
        int hit = is_pkill ? substr(names[i], target)
                           : !strcmp(base(names[i]), tbase);   /* pidof + killall: exact basename */
        if (!hit || pids[i] == self) continue;
        matched++;
        if (is_pidof) {
            if (printed++) printf(" ");
            printf("%d", pids[i]);
        } else {
            int r = kill(pids[i], sig);
            printf("%s %d (%s)\n", r == 0 ? "signalled" : "FAILED on", pids[i], names[i]);
        }
    }
    if (is_pidof && matched) printf("\n");
    return matched ? 0 : 1;
}

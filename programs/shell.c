#include "aios.h"
#include "posix.h"

#define LINE_MAX 256
static char line[LINE_MAX];
static int  line_len;
static int  shell_exit;

/* ── I/O Redirection ──────────────────────────────────── */
#define REDIR_BUF_SIZE 4096
static char redir_buf[REDIR_BUF_SIZE];
static volatile int  redir_buf_len;
static int  redir_buf_pos;
static char feed_buf[REDIR_BUF_SIZE];
static int  feed_buf_len;
static int  feed_buf_pos;

/* Saved original function pointers (stacked for recursive pipes) */
#define REDIR_STACK_MAX 4
static void (*putc_stack[REDIR_STACK_MAX])(char c);
static int    putc_stack_depth;
static int  (*getc_stack[REDIR_STACK_MAX])(void);
static int    getc_stack_depth;

/* Redirect output: capture to redir_buf */
static void redir_putc(char c) {
    if (redir_buf_len < REDIR_BUF_SIZE - 1)
        redir_buf[redir_buf_len++] = c;
}

/* Redirect input: feed from redir_buf */
static int redir_getc(void) {
    if (feed_buf_pos < feed_buf_len)
        return (unsigned char)feed_buf[feed_buf_pos++];
    return -1;
}

static void start_capture(void) {
    if (putc_stack_depth < REDIR_STACK_MAX)
        putc_stack[putc_stack_depth++] = sys->putc_direct;
    sys->putc_direct = redir_putc;
    redir_buf_len = 0;
}

static void stop_capture(void) {
    if (putc_stack_depth > 0)
        sys->putc_direct = putc_stack[--putc_stack_depth];
}

static void start_feed(void) {
    if (getc_stack_depth < REDIR_STACK_MAX)
        getc_stack[getc_stack_depth++] = sys->getc;
    sys->getc = redir_getc;
    feed_buf_pos = 0;
}

static void stop_feed(void) {
    if (getc_stack_depth > 0)
        sys->getc = getc_stack[--getc_stack_depth];
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int starts_with(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

static void print(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }
static void printc(char c) { write(STDOUT_FILENO, &c, 1); }

static void print_dec(unsigned long n) {
    char buf[20]; int i = 0;
    if (n == 0) { printc('0'); return; }
    while (n) { buf[i++] = '0' + (char)(n % 10); n /= 10; }
    while (i--) printc(buf[i]);
}

#define HIST_MAX 8
static char history[HIST_MAX][LINE_MAX];
static int hist_count = 0;
static int hist_pos = 0;

static void save_history(void) {
    if (line_len == 0) return;
    /* Don't save duplicates */
    if (hist_count > 0) {
        int prev = (hist_count - 1) % HIST_MAX;
        int same = 1;
        for (int i = 0; i < line_len; i++) {
            if (history[prev][i] != line[i]) { same = 0; break; }
        }
        if (same && history[prev][line_len] == 0) return;
    }
    int idx = hist_count % HIST_MAX;
    for (int i = 0; i < line_len; i++) history[idx][i] = line[i];
    history[idx][line_len] = 0;
    hist_count++;
}

static void clear_line(void) {
    while (line_len > 0) { print("\b \b"); line_len--; }
}

static void set_line_from_hist(int idx) {
    clear_line();
    int hi = idx % HIST_MAX;
    int i = 0;
    while (history[hi][i]) { line[i] = history[hi][i]; i++; }
    line[i] = 0;
    line_len = i;
    write(STDOUT_FILENO, line, (size_t)line_len);
}

static void read_line(void) {
    line_len = 0;
    hist_pos = hist_count;
    while (1) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r <= 0) continue;
        if (c == '\r' || c == '\n') {
            printc('\n');
            line[line_len] = '\0';
            save_history();
            return;
        }
        if ((c == 0x7f || c == '\b') && line_len > 0) {
            line_len--;
            print("\b \b");
            continue;
        }
        /* Arrow keys: ESC [ A/B */
        if (c == 0x1b) {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) continue;
            if (seq[0] == '[') {
                if (seq[1] == 'A') {  /* Up arrow */
                    if (hist_pos > 0 && hist_pos > hist_count - HIST_MAX) {
                        hist_pos--;
                        set_line_from_hist(hist_pos);
                    }
                } else if (seq[1] == 'B') {  /* Down arrow */
                    if (hist_pos < hist_count - 1) {
                        hist_pos++;
                        set_line_from_hist(hist_pos);
                    } else if (hist_pos == hist_count - 1) {
                        hist_pos = hist_count;
                        clear_line();
                    }
                }
            }
            continue;
        }
        if (c >= 0x20 && line_len < LINE_MAX - 1) {
            line[line_len++] = c;
            printc(c);
        }
    }
}

/* ── Built-in commands ────────────────────────────────── */

static void cmd_cd(const char *path) {
    if (!path || !path[0]) {
        if (chdir("/") < 0) print("cd: failed\n");
        return;
    }
    if (chdir(path) < 0) {
        print("cd: "); print(path); print(": not found\n");
    }
}


/* ── Job control ──────────────────────────────────────── */
#define MAX_JOBS 8
typedef struct {
    int active;
    int pid;
    int done;
    int exit_code;
    char name[64];
} job_t;
static job_t jobs[MAX_JOBS];
static int next_job_id = 1;

static int add_job(int pid, const char *name) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].active) {
            jobs[i].active = 1;
            jobs[i].pid = pid;
            jobs[i].done = 0;
            jobs[i].exit_code = 0;
            int k = 0;
            while (name[k] && k < 63) { jobs[i].name[k] = name[k]; k++; }
            jobs[i].name[k] = '\0';
            return i + 1; /* job IDs start at 1 */
        }
    }
    return -1;
}

static void check_jobs(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active && jobs[i].done) {
            print("["); print_dec((unsigned long)(i + 1)); print("] Done");
            print("  (exit "); print_dec((unsigned long)jobs[i].exit_code); print(")");
            print("  "); print(jobs[i].name); print("\n");
            jobs[i].active = 0;
        }
    }
}

static void cmd_jobs(void) {
    int found = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active) {
            print("["); print_dec((unsigned long)(i + 1)); print("] ");
            if (jobs[i].done) {
                print("Done     ");
            } else {
                print("Running  ");
            }
            print("pid="); print_dec((unsigned long)jobs[i].pid);
            print("  "); print(jobs[i].name); print("\n");
            found = 1;
        }
    }
    if (!found) print("No active jobs\n");
}

static void cmd_help(void) {
    print("AIOS Shell Commands:\n");
    print("  ls              - list files\n");
    print("  cat <file>      - display file contents\n");
    print("  cp <src> <dst>  - copy file\n");
    print("  mv <src> <dst>  - move/rename file\n");
    print("  rm <file>       - delete file\n");
    print("  echo <text>     - print text\n");
    print("  wc <file>       - count lines/words/bytes\n");
    print("  stat <file>     - show file info\n");
    print("  cd [dir]        - change directory\n");
    print("  pwd             - print working directory\n");
    print("  clear           - clear screen\n");
    print("  mkdir <dir>     - create directory\n");
    print("  rmdir <dir>     - remove empty directory\n");
    print("  rename <old> <new> - rename file\n");
    print("  help            - this message\n");
    print("\nRedirection:\n");
    print("  cmd > file      - redirect output to file\n");
    print("  cmd >> file     - append output to file\n");
    print("  cmd < file      - redirect input from file\n");
    print("  cmd1 | cmd2     - pipe output of cmd1 to cmd2\n");
    print("  source <file>   - run shell script\n");
    print("  ps                - list processes\n");
    print("  jobs            - list background jobs\n");
    print("  fg [%n]         - bring job to foreground\n");
    print("  sh <file>       - run shell script\n");
    print("  exit            - exit shell\n");
}

static void cmd_ls(const char *path) {
    /* Parse flags */
    int flag_l = 0, flag_a = 0;
    const char *dir_arg = path;
    /* Skip flags */
    while (dir_arg && dir_arg[0] == '-') {
        const char *f = dir_arg + 1;
        while (*f) {
            if (*f == 'l') flag_l = 1;
            else if (*f == 'a') flag_a = 1;
            f++;
        }
        /* Advance to next arg */
        while (*dir_arg && *dir_arg != ' ') dir_arg++;
        while (*dir_arg == ' ') dir_arg++;
    }
    if (!dir_arg || !dir_arg[0]) dir_arg = (void *)0;

    char cwdbuf[256];
    const char *dir;
    if (dir_arg && dir_arg[0]) {
        dir = dir_arg;
    } else {
        getcwd(cwdbuf, sizeof(cwdbuf));
        dir = cwdbuf;
    }
    DIR *d = opendir(dir);
    if (!d) { print("ls: cannot open "); print(dir); print("\n"); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* Skip hidden files unless -a */
        if (!flag_a && ent->d_name[0] == '.') continue;

        if (flag_l) {
            /* Long format: permissions size name */
            /* Get stat info */
            char fullpath[272];
            int fi = 0;
            /* Build full path for stat */
            if (dir_arg && dir_arg[0]) {
                for (int k = 0; dir_arg[k] && fi < 260; k++) fullpath[fi++] = dir_arg[k];
                if (fi > 0 && fullpath[fi-1] != '/') fullpath[fi++] = '/';
            } else {
                getcwd(fullpath, 260);
                fi = 0; while (fullpath[fi]) fi++;
                if (fi > 1) fullpath[fi++] = '/';
            }
            for (int k = 0; ent->d_name[k] && fi < 270; k++) fullpath[fi++] = ent->d_name[k];
            fullpath[fi] = '\0';

            struct stat st;
            if (stat(fullpath, &st) == 0) {
                /* Type */
                printc(S_ISDIR(st.st_mode) ? 'd' : '-');
                /* Owner */
                printc((st.st_mode & 0400) ? 'r' : '-');
                printc((st.st_mode & 0200) ? 'w' : '-');
                printc((st.st_mode & 0100) ? 'x' : '-');
                /* Group */
                printc((st.st_mode & 040) ? 'r' : '-');
                printc((st.st_mode & 020) ? 'w' : '-');
                printc((st.st_mode & 010) ? 'x' : '-');
                /* Other */
                printc((st.st_mode & 04) ? 'r' : '-');
                printc((st.st_mode & 02) ? 'w' : '-');
                printc((st.st_mode & 01) ? 'x' : '-');

                /* Owner/group names */
                print(" ");
                {
                    struct passwd *pw = getpwuid(st.st_uid);
                    if (pw) {
                        print(pw->pw_name);
                        /* Pad to 8 chars */
                        int nl = 0; const char *np = pw->pw_name; while (*np++) nl++;
                        for (int pad = nl; pad < 8; pad++) printc(' ');
                    } else {
                        print_dec((unsigned long)st.st_uid);
                        /* Rough pad */
                        if (st.st_uid < 10) print("       ");
                        else if (st.st_uid < 100) print("      ");
                        else print("     ");
                    }
                    print(" ");
                    struct group *gr = getgrgid(st.st_gid);
                    if (gr) {
                        print(gr->gr_name);
                        int gl = 0; const char *gp = gr->gr_name; while (*gp++) gl++;
                        for (int pad = gl; pad < 8; pad++) printc(' ');
                    } else {
                        print_dec((unsigned long)st.st_gid);
                        if (st.st_gid < 10) print("       ");
                        else if (st.st_gid < 100) print("      ");
                        else print("     ");
                    }
                }

                /* Size */
                print(" ");
                char szbuf[12];
                int si = 0;
                unsigned long sz = (unsigned long)st.st_size;
                if (sz == 0) { szbuf[si++] = '0'; }
                else {
                    char tmp[12]; int ti = 0;
                    while (sz > 0) { tmp[ti++] = '0' + (sz % 10); sz /= 10; }
                    while (ti > 0) szbuf[si++] = tmp[--ti];
                }
                szbuf[si] = '\0';
                /* Right-align size to 8 chars */
                for (int pad = si; pad < 8; pad++) printc(' ');
                print(szbuf);

                /* Mod time */
                if (st.st_mtime > 0) {
                    unsigned long ut = (unsigned long)st.st_mtime;
                    unsigned long days = ut / 86400;
                    unsigned long secs = ut % 86400;
                    unsigned long hrs = secs / 3600;
                    unsigned long mins = (secs % 3600) / 60;
                    unsigned long y = 1970;
                    while (1) {
                        unsigned long yd = 365;
                        if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) yd = 366;
                        if (days < yd) break;
                        days -= yd; y++;
                    }
                    static const unsigned short md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
                    unsigned long m = 0;
                    int leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
                    while (m < 12) {
                        unsigned long dd = md[m];
                        if (m == 1 && leap) dd = 29;
                        if (days < dd) break;
                        days -= dd; m++;
                    }
                    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                                    "Jul","Aug","Sep","Oct","Nov","Dec"};
                    print(" ");
                    if (m < 12) print(months[m]); else print("???");
                    printc(' ');
                    if (days + 1 < 10) printc(' ');
                    print_dec(days + 1);
                    printc(' ');
                    if (hrs < 10) printc('0'); print_dec(hrs);
                    printc(':');
                    if (mins < 10) printc('0'); print_dec(mins);
                }

                print(" ");
            } else {
                print("??????????    ");
            }
        }

        print(ent->d_name);
        if (ent->d_type == 2) printc('/');

        if (!flag_l) {
            /* Short format: pad and show size or <DIR> */
            int len = 0;
            const char *p = ent->d_name;
            while (*p++) len++;
            if (ent->d_type == 2) len++;
            for (int pad = len; pad < 18; pad++) printc(' ');
            if (ent->d_type == 2)
                print("<DIR>");
            else {
                print_dec(ent->d_size);
                print(" bytes");
            }
        }
        printc('\n');
    }
    closedir(d);
}

static void cmd_cat(const char *filename) {
    if (!filename || !filename[0]) {
        /* Read from stdin (for pipes) */
        char c;
        while (read(STDIN_FILENO, &c, 1) > 0)
            write(STDOUT_FILENO, &c, 1);
        return;
    }
    int fd = open(filename, O_RDONLY);
    if (fd < 0) { print(filename); print(": not found\n"); return; }
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(STDOUT_FILENO, buf, (size_t)n);
    close(fd);
    printc('\n');
}

static void cmd_cp(const char *args) {
    char src[128], dst[128];
    int i = 0, j = 0;
    while (args[i] && args[i] != ' ' && j < 127) src[j++] = args[i++];
    src[j] = '\0';
    while (args[i] == ' ') i++;
    j = 0;
    while (args[i] && j < 127) dst[j++] = args[i++];
    dst[j] = '\0';
    if (!dst[0]) { print("usage: cp <src> <dst>\n"); return; }
    /* If dst is a directory, append src basename */
    struct stat dst_st;
    if (stat(dst, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
        const char *base = src;
        for (const char *p = src; *p; p++) if (*p == '/') base = p + 1;
        char full[256];
        int fi = 0;
        for (int k = 0; dst[k] && fi < 250; k++) full[fi++] = dst[k];
        if (fi > 0 && full[fi-1] != '/') full[fi++] = '/';
        for (int k = 0; base[k] && fi < 255; k++) full[fi++] = base[k];
        full[fi] = '\0';
        for (int k = 0; k <= fi; k++) dst[k] = full[k];
    }
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) { print(src); print(": not found\n"); return; }
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (dfd < 0) { close(sfd); print("cp: cannot create "); print(dst); printc('\n'); return; }
    char buf[512];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) write(dfd, buf, (size_t)n);
    close(sfd);
    close(dfd);
}

static void cmd_mv(const char *args) {
    char src[128], dst[128];
    int i = 0, j = 0;
    while (args[i] && args[i] != ' ' && j < 127) src[j++] = args[i++];
    src[j] = '\0';
    while (args[i] == ' ') i++;
    j = 0;
    while (args[i] && j < 127) dst[j++] = args[i++];
    dst[j] = '\0';
    if (!dst[0]) { print("usage: mv <src> <dst>\n"); return; }
    /* If dst is a directory, append src basename */
    struct stat dst_st;
    if (stat(dst, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
        const char *base = src;
        for (const char *p = src; *p; p++) if (*p == '/') base = p + 1;
        char full[256];
        int fi = 0;
        for (int k = 0; dst[k] && fi < 250; k++) full[fi++] = dst[k];
        if (fi > 0 && full[fi-1] != '/') full[fi++] = '/';
        for (int k = 0; base[k] && fi < 255; k++) full[fi++] = base[k];
        full[fi] = '\0';
        for (int k = 0; k <= fi; k++) dst[k] = full[k];
    }
    if (rename(src, dst) < 0) {
        print("mv: failed to move "); print(src); print(" -> "); print(dst); printc('\n');
    }
}

static void cmd_stat(const char *filename) {
    struct stat st;
    if (stat(filename, &st) < 0) { print(filename); print(": not found\n"); return; }
    print("  File: "); print(filename); printc('\n');
    print("  Size: "); print_dec((unsigned long)st.st_size); print(" bytes\n");
    /* Permissions */
    char perms[11];
    perms[0] = (st.st_mode & 0x4000) ? 'd' : '-';
    perms[1] = (st.st_mode & 0x0100) ? 'r' : '-';
    perms[2] = (st.st_mode & 0x0080) ? 'w' : '-';
    perms[3] = (st.st_mode & 0x0040) ? 'x' : '-';
    perms[4] = (st.st_mode & 0x0020) ? 'r' : '-';
    perms[5] = (st.st_mode & 0x0010) ? 'w' : '-';
    perms[6] = (st.st_mode & 0x0008) ? 'x' : '-';
    perms[7] = (st.st_mode & 0x0004) ? 'r' : '-';
    perms[8] = (st.st_mode & 0x0002) ? 'w' : '-';
    perms[9] = (st.st_mode & 0x0001) ? 'x' : '-';
    perms[10] = 0;
    print("  Mode: "); print(perms); printc('\n');
    print("  Uid:  "); print_dec(st.st_uid); printc('\n');
    print("  Gid:  "); print_dec(st.st_gid); printc('\n');
    if (st.st_mtime > 0) {
        /* Convert epoch to rough date: seconds since 1970 */
        unsigned long t = st.st_mtime;
        unsigned long days = t / 86400;
        unsigned long secs = t % 86400;
        unsigned long hrs = secs / 3600;
        unsigned long mins = (secs % 3600) / 60;
        unsigned long s = secs % 60;
        /* Approximate year/month/day from days since epoch */
        unsigned long y = 1970;
        while (1) {
            unsigned long ydays = 365;
            if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ydays = 366;
            if (days < ydays) break;
            days -= ydays;
            y++;
        }
        static const unsigned short mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        unsigned long m = 0;
        int leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
        while (m < 12) {
            unsigned long md = mdays[m];
            if (m == 1 && leap) md = 29;
            if (days < md) break;
            days -= md;
            m++;
        }
        print("  Mod:  ");
        print_dec(y); printc('-');
        if (m + 1 < 10) printc('0'); print_dec(m + 1); printc('-');
        if (days + 1 < 10) printc('0'); print_dec(days + 1); printc(' ');
        if (hrs < 10) printc('0'); print_dec(hrs); printc(':');
        if (mins < 10) printc('0'); print_dec(mins); printc(':');
        if (s < 10) printc('0'); print_dec(s);
        printc('\n');
    }
}

static void cmd_wc(const char *filename) {
    int use_stdin = (!filename || !filename[0]);
    int fd = -1;
    if (!use_stdin) {
        fd = open(filename, O_RDONLY);
        if (fd < 0) { print(filename); print(": not found\n"); return; }
    }
    unsigned long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[512];
    ssize_t n;
    while ((n = use_stdin ? read(STDIN_FILENO, buf, sizeof(buf)) : read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            bytes++;
            if (buf[i] == '\n') lines++;
            if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t') in_word = 0;
            else if (!in_word) { in_word = 1; words++; }
        }
    }
    if (fd >= 0) close(fd);
    print_dec(lines); print(" "); print_dec(words); print(" ");
    print_dec(bytes);
    if (filename && filename[0]) { print(" "); print(filename); }
    printc('\n');
}

static void cmd_echo(const char *text) {
    print(text);
    printc('\n');
}

static void cmd_pwd(void) {
    char buf[64];
    getcwd(buf, sizeof(buf));
    print(buf);
    printc('\n');
}

static void cmd_clear(void) {
    print("\033[2J\033[H");
}

/* ── Parse redirection operators from line ─────────────── */
static char redir_out_file[64];
static char redir_in_file[64];
static int  redir_append;
static int  redir_pipe;
static char pipe_cmd[LINE_MAX];

static void parse_redirects(void) {
    redir_out_file[0] = 0;
    redir_in_file[0] = 0;
    redir_append = 0;
    redir_pipe = 0;
    pipe_cmd[0] = 0;

    /* Scan for | */
    for (int i = 0; line[i]; i++) {
        if (line[i] == '|') {
            line[i] = 0;
            int j = i + 1;
            while (line[j] == ' ') j++;
            int k = 0;
            while (line[j] && k < LINE_MAX - 1) pipe_cmd[k++] = line[j++];
            pipe_cmd[k] = 0;
            redir_pipe = 1;
            i--;
            while (i >= 0 && line[i] == ' ') line[i--] = 0;
            break;
        }
    }

    /* Scan for > and < */
    for (int i = 0; line[i]; i++) {
        if (line[i] == '>') {
            line[i] = 0;
            int j = i + 1;
            if (line[j] == '>') { redir_append = 1; j++; }
            while (line[j] == ' ') j++;
            int k = 0;
            while (line[j] && line[j] != ' ' && k < 63) redir_out_file[k++] = line[j++];
            redir_out_file[k] = 0;
            i--;
            while (i >= 0 && line[i] == ' ') line[i--] = 0;
            break;
        }
        if (line[i] == '<') {
            line[i] = 0;
            int j = i + 1;
            while (line[j] == ' ') j++;
            int k = 0;
            while (line[j] && line[j] != ' ' && k < 63) redir_in_file[k++] = line[j++];
            redir_in_file[k] = 0;
            i--;
            while (i >= 0 && line[i] == ' ') line[i--] = 0;
            break;
        }
    }

    line_len = 0;
    while (line[line_len]) line_len++;
}

/* ── Dispatch a single command in line[] ──────────────── */
/* ── Script execution ──────────────────────────────────── */
static void cmd_ps(void) {
    print("  PID  STAT  NAME\n");
    /* Current process (the shell) */
    print("    1  R     shell\n");
    /* List background processes from job table */
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active) {
            print("  ");
            if (jobs[i].pid < 100) print(" ");
            if (jobs[i].pid < 10) print(" ");
            print_dec((unsigned long)jobs[i].pid);
            print("  R     ");
            print(jobs[i].name);
            print("\n");
        }
    }
}

static void dispatch(void);

static void cmd_source(const char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) { print("sh: cannot open "); print(filename); print("\n"); return; }
    
    /* Read entire script into static buffer */
    static char script[REDIR_BUF_SIZE];
    int total = 0;
    ssize_t n;
    while ((n = read(fd, script + total, (size_t)(REDIR_BUF_SIZE - 1 - total))) > 0)
        total += (int)n;
    close(fd);
    script[total] = '\0';
    
    /* Execute line by line */
    int pos = 0;
    while (pos < total) {
        /* Extract one line */
        line_len = 0;
        while (pos < total && script[pos] != '\n' && script[pos] != '\r') {
            if (line_len < LINE_MAX - 1)
                line[line_len++] = script[pos];
            pos++;
        }
        line[line_len] = '\0';
        /* Skip newline chars */
        while (pos < total && (script[pos] == '\n' || script[pos] == '\r')) pos++;
        
        /* Skip empty lines and comments */
        if (line_len == 0) continue;
        if (line[0] == '#') continue;
        
        /* Echo the command */
        print("+ "); print(line); print("\n");
        
        /* Parse redirects and dispatch */
        parse_redirects();
        if (redir_in_file[0]) {
            int ifd = open(redir_in_file, O_RDONLY);
            if (ifd < 0) { print("sh: cannot open "); print(redir_in_file); print("\n"); continue; }
            redir_buf_len = (int)read(ifd, redir_buf, REDIR_BUF_SIZE - 1);
            close(ifd);
            start_feed();
        }
        if (redir_pipe || redir_out_file[0]) start_capture();
        
        dispatch();
        
        if (redir_out_file[0] && !redir_pipe) {
            stop_capture();
            if (!redir_append) unlink(redir_out_file);
            int ofd = open(redir_out_file, O_WRONLY | O_CREAT);
            if (ofd >= 0) {
                if (redir_append) lseek(ofd, 0, SEEK_END);
                write(ofd, redir_buf, (size_t)redir_buf_len);
                close(ofd);
            }
        }
        if (redir_pipe) {
            stop_capture();
            for (int fi = 0; fi < redir_buf_len && fi < REDIR_BUF_SIZE; fi++)
                feed_buf[fi] = redir_buf[fi];
            feed_buf_len = redir_buf_len;
            start_feed();
            int i = 0;
            while (pipe_cmd[i] && i < LINE_MAX - 1) { line[i] = pipe_cmd[i]; i++; }
            line[i] = 0; line_len = i;
            redir_pipe = 0;
            parse_redirects();
            if (redir_out_file[0]) start_capture();
            dispatch();
            stop_feed();
            if (redir_out_file[0]) {
                stop_capture();
                unlink(redir_out_file);
                int ofd = open(redir_out_file, O_WRONLY | O_CREAT);
                if (ofd >= 0) { write(ofd, redir_buf, (size_t)redir_buf_len); close(ofd); }
            }
        }
        if (redir_in_file[0]) stop_feed();
        
        if (shell_exit) return;
    }
}

static void dispatch(void) {
    if (line_len == 0) return;
    if (str_eq(line, "help"))           cmd_help();
    else if (str_eq(line, "ls"))        cmd_ls("");
    else if (starts_with(line, "ls "))    cmd_ls(line + 3);
    else if (str_eq(line, "pwd"))       cmd_pwd();
    else if (str_eq(line, "clear"))     cmd_clear();
    else if (str_eq(line, "exit"))      { shell_exit = 1; }
    else if (str_eq(line, "ps"))        cmd_ps();
    else if (str_eq(line, "jobs"))      cmd_jobs();
    else if (str_eq(line, "env"))       {
        for (int ei = 0; ei < 32; ei++) {
            char *e = getenv(""); /* hack — iterate manually */
            (void)e;
        }
        /* Print all env vars */
        char *ep;
        int ei = 0;
        while ((ep = _posix_environ[ei]) != (char *)0) {
            print(ep); print("\n");
            ei++;
        }
    }
    else if (starts_with(line, "export ")) {
        const char *kv = line + 7;
        /* Find = */
        char key[64], val[128];
        int ki = 0;
        while (kv[ki] && kv[ki] != '=' && ki < 63) { key[ki] = kv[ki]; ki++; }
        key[ki] = '\0';
        if (kv[ki] == '=') {
            int vi = 0;
            ki++;
            while (kv[ki] && vi < 127) { val[vi++] = kv[ki++]; }
            val[vi] = '\0';
            setenv(key, val, 1);
        }
    }
    else if (str_eq(line, "fg"))        {
        /* fg with no arg — bring most recent job */
        for (int i = MAX_JOBS - 1; i >= 0; i--) {
            if (jobs[i].active && !jobs[i].done) {
                print("["); print_dec((unsigned long)(i+1)); print("] "); print(jobs[i].name); print("\n");
                int status = 0;
                waitpid(jobs[i].pid, &status);
                jobs[i].done = 1;
                jobs[i].exit_code = status;
                break;
            }
        }
    }
    else if (starts_with(line, "fg "))  {
        int jn = 0;
        const char *p = line + 3;
        if (*p == '%') p++;
        while (*p >= '0' && *p <= '9') { jn = jn * 10 + (*p - '0'); p++; }
        if (jn > 0 && jn <= MAX_JOBS && jobs[jn-1].active && !jobs[jn-1].done) {
            print("["); print_dec((unsigned long)jn); print("] "); print(jobs[jn-1].name); print("\n");
            int status = 0;
            waitpid(jobs[jn-1].pid, &status);
            jobs[jn-1].done = 1;
            jobs[jn-1].exit_code = status;
        } else { print("fg: no such job\n"); }
    }
    else if (str_eq(line, "cd"))         cmd_cd("");
    else if (starts_with(line, "cd "))    cmd_cd(line + 3);
    else if (starts_with(line, "source ")) cmd_source(line + 7);
    else if (starts_with(line, "sh "))     cmd_source(line + 3);
    else if (str_eq(line, "cat"))        cmd_cat("");
    else if (starts_with(line, "cat ")) cmd_cat(line + 4);
    else if (starts_with(line, "cp "))  cmd_cp(line + 3);
    else if (starts_with(line, "mv "))  cmd_mv(line + 3);
    else if (starts_with(line, "rm "))  unlink(line + 3);
    else if (starts_with(line, "echo ")) cmd_echo(line + 5);
    else if (str_eq(line, "date")) {
        long (*tfn)(void) = sys->time;
        long t = tfn();
        unsigned long ut = (unsigned long)t;
        unsigned long days = ut / 86400;
        unsigned long secs = ut % 86400;
        unsigned long hrs = secs / 3600;
        unsigned long mins = (secs % 3600) / 60;
        unsigned long s = secs % 60;
        unsigned long y = 1970;
        while (1) {
            unsigned long yd = 365;
            if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) yd = 366;
            if (days < yd) break;
            days -= yd;
            y++;
        }
        static const unsigned short md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        unsigned long m = 0;
        int leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
        while (m < 12) {
            unsigned long dd = md[m];
            if (m == 1 && leap) dd = 29;
            if (days < dd) break;
            days -= dd;
            m++;
        }
        print_dec(y); printc('-');
        if (m + 1 < 10) printc('0'); print_dec(m + 1); printc('-');
        if (days + 1 < 10) printc('0'); print_dec(days + 1); printc(' ');
        if (hrs < 10) printc('0'); print_dec(hrs); printc(':');
        if (mins < 10) printc('0'); print_dec(mins); printc(':');
        if (s < 10) printc('0'); print_dec(s);
        printc('\n');
    }
    else if (starts_with(line, "stat ")) cmd_stat(line + 5);
    else if (str_eq(line, "wc"))        cmd_wc("");
    else if (starts_with(line, "wc "))  cmd_wc(line + 3);
    else if (starts_with(line, "mkdir ")) {
        if (mkdir(line + 6, 0755) < 0) print("mkdir: failed\n");
    }
    else if (starts_with(line, "rmdir ")) {
        if (rmdir(line + 6) < 0) print("rmdir: failed\n");
    }
    else if (starts_with(line, "rename ")) {
        const char *args = line + 7;
        char old[64]; int i = 0;
        while (args[i] && args[i] != ' ' && i < 63) { old[i] = args[i]; i++; }
        old[i] = '\0';
        if (args[i] == ' ') {
            if (rename(old, args + i + 1) < 0) print("rename: failed\n");
        } else print("usage: rename <old> <new>\n");
    }
    else if (starts_with(line, "exec ")) {
        char *ep = line + 5;
        while (*ep == ' ') ep++;
        const char *eargs = "";
        for (int ei = 0; ep[ei]; ei++) {
            if (ep[ei] == ' ') { eargs = ep + ei + 1; ep[ei] = '\0'; break; }
        }
        int rc = aios_exec(ep, eargs);
        if (rc < 0) {
            /* Try with /bin/ prefix */
            char ebp[272];
            int bi = 0;
            ebp[bi++] = '/'; ebp[bi++] = 'b'; ebp[bi++] = 'i'; ebp[bi++] = 'n'; ebp[bi++] = '/';
            for (int k = 0; ep[k] && bi < 271; k++) ebp[bi++] = ep[k];
            ebp[bi] = '\0';
            rc = aios_exec(ebp, eargs);
        }
        if (rc < 0) { print("exec "); print(ep); print(": command not found\n"); }
    }
    else {
        /* Check for background & */
        int bg = 0;
        {
            int ll = 0; while (line[ll]) ll++;
            while (ll > 0 && line[ll-1] == ' ') ll--;
            if (ll > 0 && line[ll-1] == '&') {
                bg = 1;
                line[ll-1] = '\0';
                while (ll > 1 && line[ll-2] == ' ') { line[ll-2] = '\0'; ll--; }
            }
        }
        /* Extract command name and args */
        char cmd[64];
        int ci = 0;
        while (line[ci] && line[ci] != ' ' && ci < 63) { cmd[ci] = line[ci]; ci++; }
        cmd[ci] = '\0';
        const char *args = "";
        for (int ai = 0; line[ai]; ai++) {
            if (line[ai] == ' ') { args = line + ai + 1; break; }
        }
        int rc;
        if (bg) {
            /* Background: use spawn instead of exec */
            char spawn_cmd[272];
            int si = 0;
            /* Try /bin/<cmd>.bin first */
            spawn_cmd[si++] = '/'; spawn_cmd[si++] = 'b'; spawn_cmd[si++] = 'i';
            spawn_cmd[si++] = 'n'; spawn_cmd[si++] = '/';
            for (int k = 0; cmd[k] && si < 259; k++) spawn_cmd[si++] = cmd[k];
            spawn_cmd[si++] = '.'; spawn_cmd[si++] = 'b'; spawn_cmd[si++] = 'i';
            spawn_cmd[si++] = 'n'; spawn_cmd[si] = '\0';
            rc = spawn(spawn_cmd, args);
            if (rc < 0) {
                /* Try cmd directly */
                rc = spawn(cmd, args);
            }
            if (rc == -2) {
                /* Script detected — can't background, run in foreground */
                print("sh: scripts cannot run in background, running in foreground\n");
                cmd_source(cmd);
                return;
            }
            if (rc >= 0) {
                int jid = add_job(rc, cmd);
                print("["); print_dec((unsigned long)jid); print("] ");
                print_dec((unsigned long)rc); print("\n");
                return;
            }
            /* Try as .sh script */
            {
                char sh[72];
                int shi = 0;
                for (int k = 0; cmd[k] && shi < 63; k++) sh[shi++] = cmd[k];
                int has_sh = (shi > 3 && sh[shi-3] == '.' && sh[shi-2] == 's' && sh[shi-1] == 'h');
                if (!has_sh) { sh[shi++] = '.'; sh[shi++] = 's'; sh[shi++] = 'h'; }
                sh[shi] = '\0';
                int sfd = open(sh, O_RDONLY);
                if (sfd < 0) sfd = open(cmd, O_RDONLY);
                if (sfd >= 0) {
                    close(sfd);
                    print("sh: scripts cannot run in background, running in foreground\n");
                    cmd_source(has_sh ? cmd : sh);
                    return;
                }
            }
            /* Fall through to not-found */
            rc = -1;
        } else {
            /* Foreground: try as-is first */
            rc = aios_exec(cmd, args);
        }
        /* -2 means script file (shebang detected) — run as source */
        if (rc == -2) { cmd_source(cmd); return; }
        /* Try with .bin extension */
        if (rc < 0) {
            char bin[72];
            int bi = 0;
            for (int k = 0; cmd[k] && bi < 63; k++) bin[bi++] = cmd[k];
            bin[bi++] = '.'; bin[bi++] = 'b'; bin[bi++] = 'i'; bin[bi++] = 'n'; bin[bi] = '\0';
            rc = aios_exec(bin, args);
        }
        /* Try /bin/<cmd> */
        if (rc < 0) {
            char bp[272];
            int bi = 0;
            bp[bi++] = '/'; bp[bi++] = 'b'; bp[bi++] = 'i'; bp[bi++] = 'n'; bp[bi++] = '/';
            for (int k = 0; cmd[k] && bi < 263; k++) bp[bi++] = cmd[k];
            bp[bi] = '\0';
            rc = aios_exec(bp, args);
        }
        /* Try /bin/<cmd>.bin */
        if (rc < 0) {
            char bp[272];
            int bi = 0;
            bp[bi++] = '/'; bp[bi++] = 'b'; bp[bi++] = 'i'; bp[bi++] = 'n'; bp[bi++] = '/';
            for (int k = 0; cmd[k] && bi < 259; k++) bp[bi++] = cmd[k];
            bp[bi++] = '.'; bp[bi++] = 'b'; bp[bi++] = 'i'; bp[bi++] = 'n'; bp[bi] = '\0';
            rc = aios_exec(bp, args);
        }
        /* Try as shell script */
        if (rc < 0) {
            /* Try cmd.sh */
            char sh[72];
            int si = 0;
            for (int k = 0; cmd[k] && si < 63; k++) sh[si++] = cmd[k];
            /* Check if it already ends with .sh */
            int has_sh = (si > 3 && sh[si-3] == '.' && sh[si-2] == 's' && sh[si-1] == 'h');
            if (!has_sh) { sh[si++] = '.'; sh[si++] = 's'; sh[si++] = 'h'; }
            sh[si] = '\0';
            int sfd = open(sh, O_RDONLY);
            if (sfd < 0) sfd = open(cmd, O_RDONLY); /* try original name */
            if (sfd >= 0) {
                close(sfd);
                cmd_source(has_sh ? cmd : sh);
                rc = 0;
            }
        }
        if (rc < 0) { print(line); print(": command not found\n"); }
    }
}

/* ── Execute with I/O redirection ─────────────────────── */
static void exec_with_redirects(void) {
    /* Expand $VARIABLES */
    {
        static char expanded[LINE_MAX];
        int ei = 0, li = 0;
        while (line[li] && ei < LINE_MAX - 2) {
            if (line[li] == '$' && line[li+1] && line[li+1] != ' ') {
                li++;
                char vname[64];
                int vi = 0;
                while (line[li] && line[li] != ' ' && line[li] != '/' &&
                       line[li] != '$' && line[li] != '\'' && line[li] != '"' &&
                       vi < 63) {
                    vname[vi++] = line[li++];
                }
                vname[vi] = '\0';
                char *val = getenv(vname);
                if (val) {
                    while (*val && ei < LINE_MAX - 2) expanded[ei++] = *val++;
                }
            } else {
                expanded[ei++] = line[li++];
            }
        }
        expanded[ei] = '\0';
        for (int k = 0; k <= ei; k++) line[k] = expanded[k];
        line_len = ei;
    }
    parse_redirects();
    /* Input redirection from file */
    if (redir_in_file[0]) {
        int fd = open(redir_in_file, O_RDONLY);
        if (fd < 0) { print("sh: cannot open "); print(redir_in_file); print("\n"); return; }
        feed_buf_len = (int)read(fd, feed_buf, REDIR_BUF_SIZE - 1);
        close(fd);
        start_feed();
    }

    /* Output capture for pipe or file redirect */
    if (redir_pipe || redir_out_file[0])
        start_capture();

    dispatch();

    /* Write captured output to file (only if no pipe follows) */
    if (redir_out_file[0] && !redir_pipe) {
        stop_capture();
        if (!redir_append) unlink(redir_out_file);
        int fd = open(redir_out_file, O_WRONLY | O_CREAT);
        if (fd >= 0) {
            if (redir_append) lseek(fd, 0, SEEK_END);
            write(fd, redir_buf, (size_t)redir_buf_len);
            close(fd);
        } else {
            print("sh: cannot create "); print(redir_out_file); print("\n");
        }
    }

    /* Pipe: feed captured output as stdin to next command (recursive) */
    if (redir_pipe) {
        stop_capture();
        /* Copy captured output to feed buffer */
        for (int fi = 0; fi < redir_buf_len && fi < REDIR_BUF_SIZE; fi++)
            feed_buf[fi] = redir_buf[fi];
        feed_buf_len = redir_buf_len;
        start_feed();
        int i = 0;
        while (pipe_cmd[i] && i < LINE_MAX - 1) { line[i] = pipe_cmd[i]; i++; }
        line[i] = 0;
        line_len = i;
        /* Recurse: parse_redirects will find further pipes */
        exec_with_redirects();
        stop_feed();
    }

    if (redir_in_file[0]) stop_feed();
}

/* ── Main loop ────────────────────────────────────────── */
AIOS_ENTRY {
    print("AIOS Shell v2.0 (POSIX)\n");
    print("Type \'help\' for commands.\n\n");
    shell_exit = 0;

    while (1) {
        check_jobs();
        print("$ ");
        read_line();
        if (line_len == 0) continue;
        exec_with_redirects();
        if (shell_exit) return 0;
    }
}

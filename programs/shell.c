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

/* Saved original function pointers */
static void (*orig_putc_direct)(char c);
static int  (*orig_getc)(void);

/* Redirect output: capture to redir_buf */
static void redir_putc(char c) {
    if (redir_buf_len < REDIR_BUF_SIZE - 1)
        redir_buf[redir_buf_len++] = c;
}

/* Redirect input: feed from redir_buf */
static int redir_getc(void) {
    if (redir_buf_pos < redir_buf_len)
        return (unsigned char)redir_buf[redir_buf_pos++];
    return -1;
}

static void start_capture(void) {
    orig_putc_direct = sys->putc_direct;
    sys->putc_direct = redir_putc;
    redir_buf_len = 0;
}

static void stop_capture(void) {
    sys->putc_direct = orig_putc_direct;
}

static void start_feed(void) {
    orig_getc = sys->getc;
    sys->getc = redir_getc;
    redir_buf_pos = 0;
}

static void stop_feed(void) {
    sys->getc = orig_getc;
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

static void read_line(void) {
    line_len = 0;
    while (1) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r <= 0) continue;
        if (c == '\r' || c == '\n') {
            printc('\n');
            line[line_len] = '\0';
            return;
        }
        if ((c == 0x7f || c == '\b') && line_len > 0) {
            line_len--;
            print("\b \b");
            continue;
        }
        if (c >= 0x20 && line_len < LINE_MAX - 1) {
            line[line_len++] = c;
            printc(c);
        }
    }
}

/* ── Built-in commands ────────────────────────────────── */

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
    print("  sh <file>       - run shell script\n");
    print("  exit            - exit shell\n");
}

static void cmd_ls(void) {
    DIR *d = opendir("/");
    if (!d) { print("ls: cannot open directory\n"); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        print(ent->d_name);
        int len = 0;
        const char *p = ent->d_name;
        while (*p++) len++;
        for (int pad = len; pad < 18; pad++) printc(' ');
        print_dec(ent->d_size);
        print(" bytes\n");
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
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) { print(src); print(": not found\n"); return; }
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (dfd < 0) { close(sfd); print("mv: cannot create "); print(dst); printc('\n'); return; }
    char buf[512];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) write(dfd, buf, (size_t)n);
    close(sfd);
    close(dfd);
    unlink(src);
}

static void cmd_stat(const char *filename) {
    struct stat st;
    if (stat(filename, &st) < 0) { print(filename); print(": not found\n"); return; }
    print("  File: "); print(filename); printc('\n');
    print("  Size: "); print_dec((unsigned long)st.st_size); print(" bytes\n");
    print("  Type: regular file\n");
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
static void dispatch(void);

static void cmd_source(const char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) { print("sh: cannot open "); print(filename); print("\n"); return; }
    
    /* Read entire script into redir_buf */
    char script[REDIR_BUF_SIZE];
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
    else if (str_eq(line, "ls"))        cmd_ls();
    else if (str_eq(line, "pwd"))       cmd_pwd();
    else if (str_eq(line, "clear"))     cmd_clear();
    else if (str_eq(line, "exit"))      { shell_exit = 1; }
    else if (starts_with(line, "source ")) cmd_source(line + 7);
    else if (starts_with(line, "sh "))     cmd_source(line + 3);
    else if (str_eq(line, "cat"))        cmd_cat("");
    else if (starts_with(line, "cat ")) cmd_cat(line + 4);
    else if (starts_with(line, "cp "))  cmd_cp(line + 3);
    else if (starts_with(line, "mv "))  cmd_mv(line + 3);
    else if (starts_with(line, "rm "))  unlink(line + 3);
    else if (starts_with(line, "echo ")) cmd_echo(line + 5);
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
    else {
        char bin[64];
        int bi = 0;
        for (int ci = 0; line[ci] && line[ci] != ' ' && bi < 55; ci++) {
            char c = line[ci];
            if (c >= 'a' && c <= 'z') c -= 32;
            bin[bi++] = c;
        }
        bin[bi++] = '.'; bin[bi++] = 'B'; bin[bi++] = 'I'; bin[bi++] = 'N'; bin[bi] = '\0';
        const char *args = "";
        for (int ci = 0; line[ci]; ci++) {
            if (line[ci] == ' ') { args = line + ci + 1; break; }
        }
        int rc = aios_exec(bin, args);
        if (rc < 0) { print(line); print(": command not found\n"); }
    }
}

/* ── Execute with I/O redirection ─────────────────────── */
static void exec_with_redirects(void) {
    parse_redirects();
    /* Input redirection from file */
    if (redir_in_file[0]) {
        int fd = open(redir_in_file, O_RDONLY);
        if (fd < 0) { print("sh: cannot open "); print(redir_in_file); print("\n"); return; }
        redir_buf_len = (int)read(fd, redir_buf, REDIR_BUF_SIZE - 1);
        close(fd);
        start_feed();
    }

    /* Output capture for pipe or file redirect */
    if (redir_pipe || redir_out_file[0])
        start_capture();

    dispatch();

    /* Write captured output to file */
    if (redir_out_file[0] && !redir_pipe) {
        stop_capture();
        if (!redir_append) unlink(redir_out_file);
        int fd = open(redir_out_file, O_WRONLY | O_CREAT);
        if (fd >= 0) {
            if (redir_append) lseek(fd, 0, SEEK_END);
            ssize_t w = write(fd, redir_buf, (size_t)redir_buf_len);
            close(fd);
        } else {
            print("sh: cannot create "); print(redir_out_file); print("\n");
        }
    }

    /* Pipe: feed captured output as stdin to next command */
    if (redir_pipe) {
        stop_capture();
        start_feed();
        int i = 0;
        while (pipe_cmd[i] && i < LINE_MAX - 1) { line[i] = pipe_cmd[i]; i++; }
        line[i] = 0;
        line_len = i;
        /* Parse any further redirects on the right side */
        redir_pipe = 0;
        parse_redirects();
        if (redir_out_file[0]) start_capture();
        dispatch();
        stop_feed();
        if (redir_out_file[0]) {
            stop_capture();
            unlink(redir_out_file);
            int fd = open(redir_out_file, O_WRONLY | O_CREAT);
            if (fd >= 0) {
                write(fd, redir_buf, (size_t)redir_buf_len);
                close(fd);
            }
        }
    }

    if (redir_in_file[0]) stop_feed();
}

/* ── Main loop ────────────────────────────────────────── */
AIOS_ENTRY {
    print("AIOS Shell v2.0 (POSIX)\n");
    print("Type \'help\' for commands.\n\n");
    shell_exit = 0;

    while (1) {
        print("$ ");
        read_line();
        if (line_len == 0) continue;
        exec_with_redirects();
        if (shell_exit) return 0;
    }
}

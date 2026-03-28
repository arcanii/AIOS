#include "aios.h"
#include "posix.h"

#define LINE_MAX 256
static char line[LINE_MAX];
static int  line_len;

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
    print("  exit            - exit shell\n");
}

static void cmd_ls(void) {
    DIR *d = opendir("/");
    if (!d) { print("ls: cannot open directory\n"); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        print(ent->d_name);
        printc('\t');
        print_dec(ent->d_size);
        printc('\n');
    }
    closedir(d);
}

static void cmd_cat(const char *filename) {
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
    int fd = open(filename, O_RDONLY);
    if (fd < 0) { print(filename); print(": not found\n"); return; }
    unsigned long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            bytes++;
            if (buf[i] == '\n') lines++;
            if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t') in_word = 0;
            else if (!in_word) { in_word = 1; words++; }
        }
    }
    close(fd);
    print_dec(lines); print(" "); print_dec(words); print(" ");
    print_dec(bytes); print(" "); print(filename); printc('\n');
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

/* ── Main loop ────────────────────────────────────────── */
AIOS_ENTRY {
    print("AIOS Shell v2.0 (POSIX)\n");
    print("Type 'help' for commands.\n\n");

    while (1) {
        print("$ ");
        read_line();
        if (line_len == 0) continue;

        if (str_eq(line, "help"))           cmd_help();
        else if (str_eq(line, "ls"))        cmd_ls();
        else if (str_eq(line, "pwd"))       cmd_pwd();
        else if (str_eq(line, "clear"))     cmd_clear();
        else if (str_eq(line, "exit"))      return 0;
        else if (starts_with(line, "cat ")) cmd_cat(line + 4);
        else if (starts_with(line, "cp "))  cmd_cp(line + 3);
        else if (starts_with(line, "mv "))  cmd_mv(line + 3);
        else if (starts_with(line, "rm "))  unlink(line + 3);
        else if (starts_with(line, "echo ")) cmd_echo(line + 5);
        else if (starts_with(line, "stat ")) cmd_stat(line + 5);
        else if (starts_with(line, "wc "))  cmd_wc(line + 3);
        else if (starts_with(line, "mkdir ")) {
            if (mkdir(line + 6, 0755) < 0) print("mkdir: failed\n");
        }
        else if (starts_with(line, "rmdir ")) {
            if (rmdir(line + 6) < 0) print("rmdir: failed\n");
        }
        else if (starts_with(line, "rename ")) {
            /* rename old new */
            const char *args = line + 7;
            char old[64]; int i = 0;
            while (args[i] && args[i] != ' ' && i < 63) { old[i] = args[i]; i++; }
            old[i] = '\0';
            if (args[i] == ' ') {
                if (rename(old, args + i + 1) < 0) print("rename: failed\n");
            } else print("usage: rename <old> <new>\n");
        }
        else {
            /* Try to exec as external program: uppercase + .BIN */
            char bin[64];
            int bi = 0;
            for (int ci = 0; line[ci] && line[ci] != ' ' && bi < 55; ci++) {
                char c = line[ci];
                if (c >= 'a' && c <= 'z') c -= 32;
                bin[bi++] = c;
            }
            bin[bi++] = '.'; bin[bi++] = 'B'; bin[bi++] = 'I'; bin[bi++] = 'N'; bin[bi] = '\0';
            /* Find args (after first space) */
            const char *args = "";
            for (int ci = 0; line[ci]; ci++) {
                if (line[ci] == ' ') { args = line + ci + 1; break; }
            }
            int rc = aios_exec(bin, args);
            if (rc < 0) { print(line); print(": command not found\n"); }
        }
    }
}

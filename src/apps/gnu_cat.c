/* cat — concatenate files (GNU-compatible)
 * Handles: stdin, multiple files, -n (line numbers), - (stdin)
 * Pure POSIX C — no AIOS headers.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

static void cat_fd(int fd, int number_lines, int *line_num) {
    char buf[4096];
    int n;
    int at_line_start = 1;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (number_lines && at_line_start) {
                char num[16];
                int ni = 0, v = *line_num;
                char tmp[10]; int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                else while (v) { tmp[ti++] = '0' + v % 10; v /= 10; }
                /* Right-justify to 6 chars */
                for (int j = 0; j < 6 - ti; j++) num[ni++] = ' ';
                while (ti--) num[ni++] = tmp[ti];
                num[ni++] = '\t';
                write(1, num, ni);
                (*line_num)++;
            }
            write(1, &buf[i], 1);
            at_line_start = (buf[i] == '\n');
        }
    }
}

int main(int argc, char *argv[]) {
    int number_lines = 0;
    int file_start = 1;
    int line_num = 1;

    /* Parse options — skip argv[0] and argv[1] (caps) */
    /* With auto-init, argv[0]=serial_ep, argv[1]=fs_ep, argv[2..]=real args */
    int real_start = 2;
    for (int i = real_start; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'n' && argv[i][2] == '\0') {
            number_lines = 1;
            real_start = i + 1;
        } else {
            break;
        }
    }

    if (real_start >= argc) {
        /* No files — read stdin */
        cat_fd(0, number_lines, &line_num);
    } else {
        for (int i = real_start; i < argc; i++) {
            if (argv[i][0] == '-' && argv[i][1] == '\0') {
                cat_fd(0, number_lines, &line_num);
            } else {
                int fd = open(argv[i], O_RDONLY);
                if (fd < 0) {
                    char err[256];
                    int len = snprintf(err, sizeof(err), "cat: %s: No such file or directory\n", argv[i]);
                    write(2, err, len);
                    continue;
                }
                cat_fd(fd, number_lines, &line_num);
                close(fd);
            }
        }
    }
    return 0;
}

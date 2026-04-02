#include "aios.h"
#include "posix.h"
AIOS_ENTRY {
    const char *filename = posix_args();
    if (!filename || !filename[0]) {
        write(STDERR_FILENO, "usage: wc <file>\n", 17);
        return 1;
    }
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        write(STDERR_FILENO, filename, strlen(filename));
        write(STDERR_FILENO, ": not found\n", 12);
        return 1;
    }
    unsigned long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            bytes++;
            if (buf[i] == '\n') lines++;
            if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }
    close(fd);
    char out[128];
    int pos = 0;
    /* Print lines */
    char tmp[12]; int ti = 0;
    unsigned long v = lines;
    if (v == 0) tmp[ti++] = '0';
    else while (v) { tmp[ti++] = '0' + (char)(v % 10); v /= 10; }
    while (ti--) out[pos++] = tmp[ti];
    out[pos++] = ' ';
    /* Print words */
    ti = 0; v = words;
    if (v == 0) tmp[ti++] = '0';
    else while (v) { tmp[ti++] = '0' + (char)(v % 10); v /= 10; }
    while (ti--) out[pos++] = tmp[ti];
    out[pos++] = ' ';
    /* Print bytes */
    ti = 0; v = bytes;
    if (v == 0) tmp[ti++] = '0';
    else while (v) { tmp[ti++] = '0' + (char)(v % 10); v /= 10; }
    while (ti--) out[pos++] = tmp[ti];
    out[pos++] = ' ';
    /* Print filename */
    for (int i = 0; filename[i]; i++) out[pos++] = filename[i];
    out[pos++] = '\n';
    write(STDOUT_FILENO, out, (size_t)pos);
    return 0;
}

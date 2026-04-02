#include "aios.h"
#include <posix.h>

static void print(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }
static void print_num(long n) {
    char buf[20]; int i = 0;
    if (n == 0) { write(STDOUT_FILENO, "0", 1); return; }
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (--i >= 0) write(STDOUT_FILENO, &buf[i], 1);
}

AIOS_ENTRY {
    int pass = 0, total = 0;
    wordexp_t w;

    total++;
    if (wordexp("hello world", &w, 0) == 0 && w.we_wordc == 2) {
        if (strcmp(w.we_wordv[0], "hello") == 0 && strcmp(w.we_wordv[1], "world") == 0)
            { pass++; print("PASS"); }
        else print("FAIL");
    } else print("FAIL");
    print(": basic word split\n");
    wordfree(&w);

    total++;
    if (wordexp("'hello world' foo", &w, 0) == 0 && w.we_wordc == 2) {
        if (strcmp(w.we_wordv[0], "hello world") == 0 && strcmp(w.we_wordv[1], "foo") == 0)
            { pass++; print("PASS"); }
        else print("FAIL");
    } else print("FAIL");
    print(": single-quoted string\n");
    wordfree(&w);

    total++;
    if (wordexp("one  two  three", &w, 0) == 0 && w.we_wordc == 3) {
        pass++; print("PASS");
    } else print("FAIL");
    print(": multiple spaces\n");
    wordfree(&w);

    if (pass == total) print("ALL PASS\n");
    return 0;
}

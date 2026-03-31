#include "aios.h"
#include <posix.h>

static void print(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }
static void print_num(long n) {
    char buf[20]; int i = 0;
    if (n < 0) { write(STDOUT_FILENO, "-", 1); n = -n; }
    if (n == 0) { write(STDOUT_FILENO, "0", 1); return; }
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (--i >= 0) write(STDOUT_FILENO, &buf[i], 1);
}

static int *_t_tests, *_t_pass;
static void check(const char *name, int cond) {
    (*_t_tests)++;
    if (cond) { (*_t_pass)++; print("  PASS: "); }
    else print("  FAIL: ");
    print(name); print("\n");
}

AIOS_ENTRY {
    int tests = 0, pass_count = 0;
    _t_tests = &tests;
    _t_pass = &pass_count;
    print("=== sscanf tests ===\n");
    {
        int a = 0, b = 0;
        int r = sscanf("42 99", "%d %d", &a, &b);
        check("sscanf %d %d count", r == 2);
        check("sscanf %d first", a == 42);
        check("sscanf %d second", b == 99);
    }
    {
        int val = 0;
        int r = sscanf("-123", "%d", &val);
        check("sscanf negative", r == 1 && val == -123);
    }
    {
        unsigned int x = 0;
        int r = sscanf("0xff", "%x", &x);
        check("sscanf %x", r == 1 && x == 255);
    }
    {
        char word[32] = {0};
        int r = sscanf("hello world", "%s", word);
        check("sscanf %s", r == 1 && strcmp(word, "hello") == 0);
    }
    {
        char c = 0;
        int r = sscanf("A", "%c", &c);
        check("sscanf %c", r == 1 && c == 'A');
    }

    print("\n=== syslog tests ===\n");
    openlog("test", 0, 0);
    syslog(6, "info message");
    syslog(3, "error message");
    closelog();
    check("syslog ran without crash", 1);

    print("\n=== strtod tests ===\n");
    {
        char *end = (char *)0;
        double v = strtod("3.14", &end);
        /* Check integer part */
        check("strtod 3.14 integer part", (int)v == 3);
        check("strtod 3.14 end advanced", end && *end == 0);
    }
    {
        double v = atof("-42.5");
        check("atof -42.5", (int)(v * 10) == -425);
    }
    {
        char *end = (char *)0;
        double v = strtod("1.5e2", &end);
        check("strtod 1.5e2 = 150", (int)v == 150);
    }
    {
        double v = atof("  +0.001");
        /* 0.001 * 10000 = 10 */
        check("atof +0.001", (int)(v * 10000) == 10);
    }

    print("\n=== results ===\n");
    print_num(pass_count); print("/"); print_num(tests); print(" passed\n");
    if (pass_count == tests) print("ALL PASS\n");
    else print("SOME FAILURES\n");
    return 0;
}

#include "aios.h"
#include <posix.h>

static void print(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }

AIOS_ENTRY {
    int pass = 0, total = 0;
    regex_t re;
    regmatch_t pm[1];

    total++; regcomp(&re, "hello", 0);
    if (regexec(&re, "say hello world", 1, pm, 0) == 0) { pass++; print("PASS"); } else print("FAIL");
    print(": literal match\n");

    total++; regcomp(&re, "^hello", 0);
    if (regexec(&re, "hello world", 1, pm, 0) == 0) { pass++; print("PASS"); } else print("FAIL");
    print(": anchored ^ match\n");

    total++; regcomp(&re, "^hello", 0);
    if (regexec(&re, "say hello", 1, pm, 0) != 0) { pass++; print("PASS"); } else print("FAIL");
    print(": anchored ^ reject\n");

    total++; regcomp(&re, "world$", 0);
    if (regexec(&re, "hello world", 1, pm, 0) == 0) { pass++; print("PASS"); } else print("FAIL");
    print(": anchored $ match\n");

    total++; regcomp(&re, "h.llo", 0);
    if (regexec(&re, "hello", 1, pm, 0) == 0) { pass++; print("PASS"); } else print("FAIL");
    print(": dot wildcard\n");

    total++; regcomp(&re, "ab*c", 0);
    if (regexec(&re, "ac", 1, pm, 0) == 0) { pass++; print("PASS"); } else print("FAIL");
    print(": b* zero matches\n");

    total++; regcomp(&re, "ab*c", 0);
    if (regexec(&re, "abbbbc", 1, pm, 0) == 0) { pass++; print("PASS"); } else print("FAIL");
    print(": b* multiple matches\n");

    if (pass == total) print("ALL PASS\n");
    return 0;
}

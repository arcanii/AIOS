#include "aios.h"
#include <posix.h>

static void print(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }

AIOS_ENTRY {
    int pass = 0, total = 0;

    total++; if (fnmatch("*.txt", "hello.txt", 0) == 0) { pass++; print("PASS"); } else print("FAIL");
    print(": *.txt matches hello.txt\n");

    total++; if (fnmatch("*.txt", "hello.c", 0) != 0) { pass++; print("PASS"); } else print("FAIL");
    print(": *.txt rejects hello.c\n");

    total++; if (fnmatch("h?llo", "hello", 0) == 0) { pass++; print("PASS"); } else print("FAIL");
    print(": h?llo matches hello\n");

    total++; if (fnmatch("*", "anything", 0) == 0) { pass++; print("PASS"); } else print("FAIL");
    print(": * matches anything\n");

    total++; if (fnmatch("exact", "exact", 0) == 0) { pass++; print("PASS"); } else print("FAIL");
    print(": exact matches exact\n");

    total++; if (fnmatch("exact", "other", 0) != 0) { pass++; print("PASS"); } else print("FAIL");
    print(": exact rejects other\n");

    total++; if (fnmatch("*.bin", "shell.bin", 0) == 0) { pass++; print("PASS"); } else print("FAIL");
    print(": *.bin matches shell.bin\n");

    if (pass == total) print("ALL PASS\n");
    return 0;
}

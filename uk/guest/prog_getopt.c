/*
 * prog_getopt.c -- exercises getopt (POSIX option parsing) + qsort (generic sort), the two
 * pure-libc gaps real sbase/sort/ls lean on. Real C via the -nostdinc shadow headers (M3e.5).
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int fails = 0;

    /* getopt over a synthetic argv: two flags, one option-with-argument, then a non-option. */
    char *gv[] = { "prog", "-a", "-b", "-f", "hello", "world", 0 };
    int gc = 6, aflag = 0, bflag = 0, c;
    char *fval = 0;
    while ((c = getopt(gc, gv, "abf:")) != -1) {
        switch (c) {
        case 'a': aflag = 1; break;
        case 'b': bflag = 1; break;
        case 'f': fval = optarg; break;
        default:  fails++; break;
        }
    }
    printf("getopt: a=%d b=%d f=%s rest=%s\n", aflag, bflag,
           fval ? fval : "(none)", gv[optind] ? gv[optind] : "(none)");
    if (!aflag || !bflag || !fval || strcmp(fval, "hello") != 0 ||
        !gv[optind] || strcmp(gv[optind], "world") != 0) fails++;

    /* qsort of ints */
    int nums[] = { 5, 3, 9, 1, 7, 2, 8, 4, 6, 0 };
    int n = (int)(sizeof nums / sizeof nums[0]);
    qsort(nums, (size_t)n, sizeof nums[0], cmp_int);
    printf("qsort ints:");
    for (int i = 0; i < n; i++) printf(" %d", nums[i]);
    printf("\n");
    for (int i = 1; i < n; i++) if (nums[i] < nums[i - 1]) fails++;

    /* qsort of strings */
    const char *words[] = { "pear", "apple", "fig", "banana", "cherry" };
    int wn = (int)(sizeof words / sizeof words[0]);
    qsort(words, (size_t)wn, sizeof words[0], cmp_str);
    printf("qsort strings:");
    for (int i = 0; i < wn; i++) printf(" %s", words[i]);
    printf("\n");
    for (int i = 1; i < wn; i++) if (strcmp(words[i], words[i - 1]) < 0) fails++;

    printf("prog_getopt: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

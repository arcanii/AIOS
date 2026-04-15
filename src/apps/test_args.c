/* test_args.c -- enhanced argv dump with progpath */
#include <stdio.h>

extern char aios_progpath[];

int main(int argc, char **argv)
{
    int i;
    printf("progpath=[%s]\n", aios_progpath);
    printf("argc=%d\n", argc);
    for (i = 0; i < argc; i++)
        printf("  argv[%d]=[%s]\n", i, argv[i] ? argv[i] : "(null)");
    printf("  argv[-1]=[%s]\n", argv[-1] ? argv[-1] : "(null)");
    return 0;
}

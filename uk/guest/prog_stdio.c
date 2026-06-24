/*
 * prog_stdio.c -- exercises FILE* buffered stdio (M3e step 2), real C, -nostdinc shadow headers.
 *
 * Writes a file with fopen("w")/fprintf/fputs/fwrite, reads it back with fopen("r")/fgets, echoes
 * via buffered printf, and (with "-") echoes stdin with getchar/putchar. Diagnostics go to stderr
 * (fprintf) so stdout stays clean for piping. Proves the stdio layer real sbase/dash lean on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *path = "/tmp/aios_stdio.txt";

    FILE *out = fopen(path, "w");
    if (!out) { fprintf(stderr, "prog_stdio: fopen(w) failed\n"); return 1; }
    fprintf(out, "line %d: %s\n", 1, "fprintf to a file");
    fputs("line 2: fputs to a file\n", out);
    const char *blob = "line 3: fwrite to a file\n";
    fwrite(blob, 1, strlen(blob), out);
    fclose(out);

    FILE *in = fopen(path, "r");
    if (!in) { fprintf(stderr, "prog_stdio: fopen(r) failed\n"); return 1; }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, in)) { n++; printf("  got: %s", line); }   /* line keeps its \n */
    fclose(in);
    fprintf(stderr, "prog_stdio: wrote + read back %d lines via FILE* stdio\n", n);

    if (argc >= 2 && strcmp(argv[1], "-") == 0) {     /* echo stdin (a pipe) char by char */
        int c, bytes = 0;
        while ((c = getchar()) != EOF) { putchar(c); bytes++; }
        fprintf(stderr, "prog_stdio: echoed %d bytes from stdin\n", bytes);
    }
    return (n == 3) ? 0 : 2;
}

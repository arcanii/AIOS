/*
 * prog_dir.c -- exercises directory streams (M3e.6): opendir/readdir/closedir over the new
 * AIOS_SYS_GETDENTS. Builds a temp directory, lists it, sorts the names (qsort), checks the
 * entries + the subdir's d_type, and that opendir on a non-directory fails with ENOTDIR. This is
 * what ls needs. Real C via the -nostdinc shadow headers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int main(void) {
    int fails = 0;
    const char *dir = "/tmp/aios_dir_test";
    const char *names[] = { "alpha", "bravo", "charlie" };
    char path[256];

    /* clean slate (ignore errors) */
    for (int i = 0; i < 3; i++) { snprintf(path, sizeof path, "%s/%s", dir, names[i]); unlink(path); }
    snprintf(path, sizeof path, "%s/subdir", dir); rmdir(path);
    rmdir(dir);

    if (mkdir(dir, 0755) != 0) { printf("mkdir: %s\n", strerror(errno)); return 1; }
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        FILE *fp = fopen(path, "w");
        if (fp) { fputs("x\n", fp); fclose(fp); } else fails++;
    }
    snprintf(path, sizeof path, "%s/subdir", dir);
    if (mkdir(path, 0755) != 0) fails++;

    DIR *d = opendir(dir);
    if (!d) { printf("opendir: %s\n", strerror(errno)); return 1; }

    char *found[64]; int nf = 0, subdir_type = -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (nf < 64) found[nf++] = strdup(de->d_name);
        if (strcmp(de->d_name, "subdir") == 0) subdir_type = de->d_type;
    }
    closedir(d);

    qsort(found, (size_t)nf, sizeof found[0], cmp_str);
    printf("readdir found %d entries:", nf);
    for (int i = 0; i < nf; i++) printf(" %s", found[i]);
    printf("\n");

    /* expect exactly: alpha bravo charlie subdir */
    const char *want[] = { "alpha", "bravo", "charlie", "subdir" };
    if (nf != 4) fails++;
    else for (int i = 0; i < 4; i++) if (strcmp(found[i], want[i]) != 0) fails++;

    /* d_type: DT_DIR is ideal; DT_UNKNOWN is acceptable (some filesystems require a stat fallback);
     * any other value is wrong. */
    printf("subdir d_type = %d (%s)\n", subdir_type,
           subdir_type == DT_DIR ? "DT_DIR" : subdir_type == DT_UNKNOWN ? "DT_UNKNOWN" : "WRONG");
    if (subdir_type != DT_DIR && subdir_type != DT_UNKNOWN) fails++;

    /* opendir on a regular file must fail with ENOTDIR */
    snprintf(path, sizeof path, "%s/alpha", dir);
    DIR *nd = opendir(path);
    if (nd) { printf("opendir(file) unexpectedly succeeded\n"); closedir(nd); fails++; }
    else if (errno != ENOTDIR) { printf("opendir(file): expected ENOTDIR, got %s\n", strerror(errno)); fails++; }

    /* cleanup */
    for (int i = 0; i < 3; i++) { snprintf(path, sizeof path, "%s/%s", dir, names[i]); unlink(path); }
    snprintf(path, sizeof path, "%s/subdir", dir); rmdir(path);
    rmdir(dir);

    printf("prog_dir: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

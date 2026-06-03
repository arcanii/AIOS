/* test_mmap -- v0.4.144 file-backed mmap smoke (architecture A)
 *
 * 1. Create a backing file with known content.
 * 2. mmap(MAP_PRIVATE): the mapped bytes must equal the file; a write to a
 *    private map must NOT reach disk.
 * 3. mmap(MAP_SHARED, PROT_WRITE): modify a region, msync, munmap; the change
 *    must persist to the file.
 *
 * Note: AIOS open() is read-only, but mmap write-back rides the path-based
 * FS_PWRITE, so a MAP_SHARED + PROT_WRITE map persists -- mmap write
 * permission is deliberately decoupled from the fd open mode here.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

static int run = 0, pass = 0;
static void check(const char *name, int cond) {
    run++;
    if (cond) { pass++; printf("  PASS: %s\n", name); }
    else      { printf("  FAIL: %s\n", name); }
}

int main(void) {
    const char *path = "/tmp/test_mmap.dat";
    char content[200];
    for (int i = 0; i < (int)sizeof(content); i++)
        content[i] = (char)('A' + (i % 26));
    int clen = (int)sizeof(content);
    char buf[200];

    printf("=== AIOS file-backed mmap test (A) ===\n");

    /* [1] create the backing file */
    printf("\n[1] Create backing file\n");
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check("open(O_CREAT|O_WRONLY) >= 0", fd >= 0);
    if (fd < 0) { printf("\n=== %d/%d ===\nMMAP-DONE\n", pass, run); return 1; }
    check("write content", write(fd, content, clen) == clen);
    close(fd);

    /* [2] MAP_PRIVATE read: mapped bytes equal the file */
    printf("\n[2] MAP_PRIVATE read + non-persistent write\n");
    fd = open(path, O_RDONLY);
    check("open(O_RDONLY) >= 0", fd >= 0);
    char *rp = mmap(NULL, clen, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    check("mmap(MAP_PRIVATE) ok", rp != MAP_FAILED && rp != NULL);
    if (rp != MAP_FAILED && rp != NULL) {
        check("mapped bytes match file", memcmp(rp, content, clen) == 0);
        rp[0] = '#';                       /* private write -- must not persist */
        munmap(rp, clen);
    }
    close(fd);
    fd = open(path, O_RDONLY);
    check("MAP_PRIVATE write did NOT persist",
          read(fd, buf, clen) == clen && buf[0] == content[0]);
    close(fd);

    /* [3] MAP_SHARED write-back via msync */
    printf("\n[3] MAP_SHARED write-back\n");
    fd = open(path, O_RDONLY);
    char *sp = mmap(NULL, clen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check("mmap(MAP_SHARED) ok", sp != MAP_FAILED && sp != NULL);
    if (sp != MAP_FAILED && sp != NULL) {
        sp[0] = '0'; sp[1] = '1'; sp[2] = '2'; sp[3] = '3';
        check("msync rc==0", msync(sp, clen, MS_SYNC) == 0);
        munmap(sp, clen);
    }
    close(fd);
    fd = open(path, O_RDONLY);
    check("MAP_SHARED write persisted",
          read(fd, buf, clen) == clen
          && buf[0] == '0' && buf[1] == '1' && buf[2] == '2' && buf[3] == '3'
          && buf[4] == content[4]);        /* untouched tail intact */
    close(fd);

    /* [4] cleanup */
    unlink(path);

    printf("\n=== Results: %d/%d passed ===\n", pass, run);
    printf("MMAP-DONE\n");
    return (pass == run) ? 0 : 1;
}

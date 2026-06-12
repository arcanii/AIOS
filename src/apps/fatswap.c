/*
 * fatswap.c -- flash-over-network CLI: rewrite KERNEL8.IMG on the FAT32
 * boot partition from a file on the system disk (usually pushed there via
 * netconsole __put). The heavy lifting -- and the crash-safe write ordering
 * -- lives in the root task (FS_FATSWAP, src/fat32.c); this is just the
 * trigger + report. Root only. See scripts/pi_flash.py for the full
 * push + swap + verify + reboot deploy flow.
 *
 * Usage: fatswap <absolute-path>
 * Crash-safety test flags (QEMU tests; each stops between write phases):
 *   fatswap --abort-after-data   <path>
 *   fatswap --abort-after-fat    <path>
 *   fatswap --abort-after-dirent <path>
 */
#include <stdio.h>
#include <string.h>
#include "aios_posix.h"

#define FS_FATSWAP 23

static const char *err_str(long rc) {
    switch (rc) {
    case -1:  return "permission denied (root only)";
    case -2:  return "no FAT boot partition on this disk";
    case -3:  return "unsupported or corrupt FAT32 BPB";
    case -4:  return "source file missing, empty, or unreadable";
    case -5:  return "KERNEL8.IMG not found in the FAT root directory";
    case -6:  return "not enough free clusters on the boot partition";
    case -7:  return "sector I/O failed";
    case -8:  return "readback verification FAILED after commit";
    case -9:  return "corrupt FAT chain";
    case -10: return "source file too large";
    default:  return "unknown error";
    }
}

static void sha_hex(const unsigned char *sha, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i*2]   = hx[sha[i] >> 4];
        out[i*2+1] = hx[sha[i] & 0xF];
    }
    out[64] = 0;
}

int main(int argc, char *argv[]) {
    const char *path = NULL;
    unsigned long abort_phase = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--abort-after-data") == 0)   abort_phase = 1;
        else if (strcmp(argv[i], "--abort-after-fat") == 0)    abort_phase = 2;
        else if (strcmp(argv[i], "--abort-after-dirent") == 0) abort_phase = 3;
        else path = argv[i];
    }
    if (!path || path[0] != '/') {
        fprintf(stderr, "usage: fatswap <absolute-path>\n");
        return 2;
    }

    seL4_CPtr fs = aios_get_fs_ep();
    if (!fs) {
        fprintf(stderr, "fatswap: no fs endpoint\n");
        return 1;
    }

    int len = (int)strlen(path);
    if (len > 127) {
        fprintf(stderr, "fatswap: path too long\n");
        return 2;
    }

    seL4_SetMR(0, (seL4_Word)len);
    seL4_SetMR(1, (seL4_Word)abort_phase);
    int mr = 2;
    seL4_Word w = 0;
    for (int i = 0; i < len; i++) {
        w |= ((seL4_Word)(unsigned char)path[i]) << ((i % 8) * 8);
        if (i % 8 == 7) { seL4_SetMR(mr++, w); w = 0; }
    }
    if (len % 8) seL4_SetMR(mr++, w);

    /* The swap runs synchronously in the root task (seconds on real
     * hardware: CMD25 data write + FAT updates + full readback hash). */
    seL4_Call(fs, seL4_MessageInfo_new(FS_FATSWAP, 0, 0, mr));

    long rc = (long)seL4_GetMR(0);
    unsigned long bytes    = (unsigned long)seL4_GetMR(1);
    unsigned long clusters = (unsigned long)seL4_GetMR(2);
    unsigned long first    = (unsigned long)seL4_GetMR(3);
    unsigned char sha_src[32], sha_disk[32];
    for (int i = 0; i < 4; i++) {
        seL4_Word v = seL4_GetMR(4 + i);
        for (int j = 0; j < 8; j++)
            sha_src[i*8+j] = (unsigned char)(v >> (j*8));
    }
    for (int i = 0; i < 4; i++) {
        seL4_Word v = seL4_GetMR(8 + i);
        for (int j = 0; j < 8; j++)
            sha_disk[i*8+j] = (unsigned char)(v >> (j*8));
    }

    if (rc != 0) {
        fprintf(stderr, "fatswap: FAIL (%ld): %s\n", rc, err_str(rc));
        return 1;
    }

    char hex[65];
    if (abort_phase) {
        printf("fatswap: DEBUG ABORT phase %lu (%lu bytes staged, %lu clusters, first=%lu)\n",
               abort_phase, bytes, clusters, first);
        sha_hex(sha_src, hex);
        printf("fatswap: sha256 src  = %s\n", hex);
        return 0;
    }
    printf("fatswap: %lu bytes in %lu clusters (first=%lu)\n",
           bytes, clusters, first);
    sha_hex(sha_src, hex);
    printf("fatswap: sha256 src  = %s\n", hex);
    sha_hex(sha_disk, hex);
    printf("fatswap: sha256 disk = %s\n", hex);
    printf("fatswap: OK\n");
    return 0;
}

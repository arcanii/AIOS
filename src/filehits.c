/*
 * filehits.c -- v0.4.114 access-frequency profiler
 *
 * Bounded fixed table; LRU-evicted on overflow. Hooked from
 * vfs_read / vfs_pread (calls filehits_record). /proc/filehits
 * dumps the top N entries by access count.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "aios/filehits.h"

typedef struct {
    char     path[FILEHITS_PATH_MAX];
    uint32_t opens;
    uint64_t bytes_read;
    uint32_t last_tick;
    uint8_t  used;
} filehits_entry_t;

static filehits_entry_t entries[FILEHITS_MAX];
static uint32_t global_tick = 0;

/* Skip /proc paths -- virtual fs, would pollute its own profile. */
static int should_skip(const char *path) {
    if (!path) return 1;
    if (path[0] == '/' && path[1] == 'p' && path[2] == 'r'
        && path[3] == 'o' && path[4] == 'c'
        && (path[5] == '/' || path[5] == '\0')) return 1;
    return 0;
}

static int find_or_alloc(const char *path) {
    int oldest_idx = -1;
    uint32_t oldest_tick = 0xffffffffu;
    int free_idx = -1;
    for (int i = 0; i < FILEHITS_MAX; i++) {
        if (!entries[i].used) {
            if (free_idx < 0) free_idx = i;
            continue;
        }
        if (strcmp(entries[i].path, path) == 0) return i;
        if (entries[i].last_tick < oldest_tick) {
            oldest_tick = entries[i].last_tick;
            oldest_idx = i;
        }
    }
    int idx = (free_idx >= 0) ? free_idx : oldest_idx;
    if (idx < 0) return -1;
    /* (Re)initialize the slot. */
    int n = 0;
    while (path[n] && n < FILEHITS_PATH_MAX - 1) {
        entries[idx].path[n] = path[n];
        n++;
    }
    entries[idx].path[n] = '\0';
    entries[idx].opens = 0;
    entries[idx].bytes_read = 0;
    entries[idx].used = 1;
    return idx;
}

void filehits_record(const char *path, int bytes) {
    if (should_skip(path)) return;
    int idx = find_or_alloc(path);
    if (idx < 0) return;
    entries[idx].opens++;
    if (bytes > 0) entries[idx].bytes_read += (uint64_t)bytes;
    entries[idx].last_tick = ++global_tick;
}

int filehits_format(char *buf, int bufsize, int top_n) {
    /* Build an index array sorted by opens desc. Simple selection-
     * sort over the small fixed table. */
    int idx[FILEHITS_MAX];
    int n = 0;
    for (int i = 0; i < FILEHITS_MAX; i++) {
        if (entries[i].used) idx[n++] = i;
    }
    /* Selection sort -- N is at most 128, fine. */
    for (int i = 0; i < n; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) {
            if (entries[idx[j]].opens > entries[idx[best]].opens) best = j;
        }
        if (best != i) { int t = idx[i]; idx[i] = idx[best]; idx[best] = t; }
    }

    int w = 0;
    w += snprintf(buf + w, bufsize - w,
                  "%-48s %8s %12s %8s\n",
                  "path", "opens", "bytes", "last");
    int limit = (top_n > 0 && top_n < n) ? top_n : n;
    for (int i = 0; i < limit && w < bufsize - 1; i++) {
        filehits_entry_t *e = &entries[idx[i]];
        w += snprintf(buf + w, bufsize - w,
                      "%-48s %8u %12llu %8u\n",
                      e->path,
                      e->opens,
                      (unsigned long long)e->bytes_read,
                      e->last_tick);
    }
    if (w < bufsize) buf[w] = '\0';
    return w;
}

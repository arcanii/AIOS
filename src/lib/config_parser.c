/*
 * config_parser.c -- AIOS shared key=value config parser
 * v0.4.80: parsed by both root task and user programs
 *
 * Format: key=value lines, # comments, blank lines skipped.
 * No malloc -- all static buffers.
 */
#include "aios/config.h"

/* ---- Global config instances (defaults) ---- */
cfg_hostname_t sys_hostname = { .name = "aios" };
cfg_net_t      sys_net      = {
    .ip      = { 10, 0, 2, 15 },
    .gateway = { 10, 0, 2, 2 },
    .mask    = { 255, 255, 255, 0 },
    .loaded  = 0
};
cfg_env_t      sys_env      = { .count = 0, .loaded = 0 };

/* Runtime network arrays (referenced by net_stack.c, net_tcp.c) */
uint8_t net_cfg_ip[4]   = { 10, 0, 2, 15 };
uint8_t net_cfg_gw[4]   = { 10, 0, 2, 2 };
uint8_t net_cfg_mask[4]  = { 255, 255, 255, 0 };

/* ---- Helper: compare two strings ---- */
static int cfg_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == 0 && *b == 0);
}

/* ---- Parse key=value buffer ---- */
int cfg_parse_kv(const char *buf, int len, cfg_file_t *out) {
    out->count = 0;
    int pos = 0;
    while (pos < len && out->count < CFG_MAX_ENTRIES) {
        /* Skip whitespace and blank lines */
        while (pos < len && (buf[pos] == ' ' || buf[pos] == '\t'
               || buf[pos] == '\r' || buf[pos] == '\n'))
            pos++;
        if (pos >= len) break;

        /* Skip comment lines */
        if (buf[pos] == '#') {
            while (pos < len && buf[pos] != '\n') pos++;
            continue;
        }

        /* Extract key (up to = or newline) */
        cfg_entry_t *e = &out->entries[out->count];
        int ki = 0;
        while (pos < len && buf[pos] != '=' && buf[pos] != '\n'
               && ki < CFG_MAX_KEY - 1) {
            e->key[ki++] = buf[pos++];
        }
        e->key[ki] = 0;

        /* Must have = separator */
        if (pos >= len || buf[pos] != '=') {
            while (pos < len && buf[pos] != '\n') pos++;
            continue;
        }
        pos++;  /* skip = */

        /* Extract value (up to newline or end) */
        int vi = 0;
        while (pos < len && buf[pos] != '\n' && buf[pos] != '\r'
               && vi < CFG_MAX_VALUE - 1) {
            e->value[vi++] = buf[pos++];
        }
        e->value[vi] = 0;

        out->count++;
    }
    return out->count;
}

/* ---- Look up key in parsed config ---- */
const char *cfg_get(const cfg_file_t *cfg, const char *key) {
    for (int i = 0; i < cfg->count; i++) {
        if (cfg_streq(cfg->entries[i].key, key))
            return cfg->entries[i].value;
    }
    return 0;
}

/* ---- Parse dotted-quad IP string ---- */
int cfg_parse_ip(const char *str, uint8_t ip[4]) {
    if (!str) return -1;
    int octet = 0, val = 0, digits = 0;
    for (int i = 0; ; i++) {
        char c = str[i];
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            digits++;
            if (val > 255) return -1;
        } else if (c == '.' || c == 0) {
            if (digits == 0 || octet >= 4) return -1;
            ip[octet++] = (uint8_t)val;
            val = 0;
            digits = 0;
            if (c == 0) break;
        } else {
            return -1;
        }
    }
    return (octet == 4) ? 0 : -1;
}

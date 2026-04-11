/*
 * boot_config.c -- AIOS boot-time configuration loader
 * v0.4.80: reads /etc/hostname, /etc/network.conf, /etc/environment
 *
 * Runs in root task context after VFS mount, before server threads start.
 * Uses vfs_read() directly (no IPC to fs_server needed).
 */
#include "aios/config.h"
#include "aios/vfs.h"
#include <stdio.h>

/* ---- Load /etc/hostname ---- */
void cfg_load_hostname(cfg_hostname_t *h) {
    char buf[128];
    int len = vfs_read("/etc/hostname", buf, sizeof(buf) - 1);
    if (len <= 0) return;  /* keep default "aios" */
    buf[len] = 0;
    /* Strip trailing newline/whitespace */
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'
           || buf[len-1] == ' '))
        buf[--len] = 0;
    if (len > 0) {
        int i = 0;
        while (buf[i] && i < CFG_HOSTNAME_MAX - 1) {
            h->name[i] = buf[i]; i++;
        }
        h->name[i] = 0;
    }
}

/* ---- Load /etc/network.conf ---- */
void cfg_load_network(cfg_net_t *n) {
    char buf[512];
    int len = vfs_read("/etc/network.conf", buf, sizeof(buf) - 1);
    if (len <= 0) return;  /* keep defaults */
    buf[len] = 0;

    cfg_file_t cfg;
    cfg_parse_kv(buf, len, &cfg);

    const char *v;
    if ((v = cfg_get(&cfg, "ip")) != 0)
        cfg_parse_ip(v, n->ip);
    if ((v = cfg_get(&cfg, "gateway")) != 0)
        cfg_parse_ip(v, n->gateway);
    if ((v = cfg_get(&cfg, "mask")) != 0)
        cfg_parse_ip(v, n->mask);

    /* Copy to runtime arrays used by net_stack.c */
    for (int i = 0; i < 4; i++) {
        net_cfg_ip[i]   = n->ip[i];
        net_cfg_gw[i]   = n->gateway[i];
        net_cfg_mask[i]  = n->mask[i];
    }
    n->loaded = 1;
}

/* ---- Load /etc/environment ---- */
void cfg_load_environment(cfg_env_t *e) {
    char buf[1024];
    int len = vfs_read("/etc/environment", buf, sizeof(buf) - 1);
    if (len <= 0) return;  /* keep defaults */
    buf[len] = 0;

    cfg_file_t cfg;
    cfg_parse_kv(buf, len, &cfg);

    e->count = 0;
    for (int i = 0; i < cfg.count && e->count < CFG_MAX_ENV; i++) {
        /* Format as "KEY=value" */
        int ki = 0, vi = 0, oi = 0;
        char *dst = e->vars[e->count];
        while (cfg.entries[i].key[ki] && oi < CFG_ENV_MAX - 2) {
            dst[oi++] = cfg.entries[i].key[ki++];
        }
        dst[oi++] = '=';
        while (cfg.entries[i].value[vi] && oi < CFG_ENV_MAX - 1) {
            dst[oi++] = cfg.entries[i].value[vi++];
        }
        dst[oi] = 0;
        e->count++;
    }
    e->loaded = 1;
}

/* ---- Master boot config loader ---- */
void boot_load_config(void) {
    cfg_load_hostname(&sys_hostname);
    printf("[boot] hostname: %s\n", sys_hostname.name);

    cfg_load_network(&sys_net);
    if (sys_net.loaded) {
        printf("[boot] net: %d.%d.%d.%d gw %d.%d.%d.%d mask %d.%d.%d.%d\n",
               sys_net.ip[0], sys_net.ip[1], sys_net.ip[2], sys_net.ip[3],
               sys_net.gateway[0], sys_net.gateway[1],
               sys_net.gateway[2], sys_net.gateway[3],
               sys_net.mask[0], sys_net.mask[1],
               sys_net.mask[2], sys_net.mask[3]);
    } else {
        printf("[boot] net: using defaults (no /etc/network.conf)\n");
    }

    cfg_load_environment(&sys_env);
    printf("[boot] env: %d variables%s\n",
           sys_env.count,
           sys_env.loaded ? "" : " (defaults)");
}

#ifndef AIOS_CONFIG_H
#define AIOS_CONFIG_H
/*
 * AIOS configuration system -- v0.4.80
 * Shared key=value parser and boot-time config structs.
 * Used by root task (vfs_read direct) and user programs (fetch_file IPC).
 */
#include <stdint.h>

/* Limits for config parsing */
#define CFG_MAX_KEY       32
#define CFG_MAX_VALUE    128
#define CFG_MAX_ENTRIES   16
#define CFG_HOSTNAME_MAX  64
#define CFG_MAX_ENV       16
#define CFG_ENV_MAX      128

/* Single key=value entry */
typedef struct {
    char key[CFG_MAX_KEY];
    char value[CFG_MAX_VALUE];
} cfg_entry_t;

/* Parsed key=value file */
typedef struct {
    cfg_entry_t entries[CFG_MAX_ENTRIES];
    int count;
} cfg_file_t;

/* Hostname config (from /etc/hostname) */
typedef struct {
    char name[CFG_HOSTNAME_MAX];
} cfg_hostname_t;

/* Network config (from /etc/network.conf) */
typedef struct {
    uint8_t ip[4];
    uint8_t gateway[4];
    uint8_t mask[4];
    int loaded;
} cfg_net_t;

/* Environment config (from /etc/environment) */
typedef struct {
    char vars[CFG_MAX_ENV][CFG_ENV_MAX];
    int count;
    int loaded;
} cfg_env_t;

/* ---- Parser API (config_parser.c) ---- */
int cfg_parse_kv(const char *buf, int len, cfg_file_t *out);
const char *cfg_get(const cfg_file_t *cfg, const char *key);
int cfg_parse_ip(const char *str, uint8_t ip[4]);

/* ---- Boot-time loaders (boot_config.c, root task only) ---- */
void cfg_load_hostname(cfg_hostname_t *h);
void cfg_load_network(cfg_net_t *n);
void cfg_load_environment(cfg_env_t *e);
void boot_load_config(void);

/* ---- Global config state (defined in config_parser.c) ---- */
extern cfg_hostname_t  sys_hostname;
extern cfg_net_t       sys_net;
extern cfg_env_t       sys_env;

/* Runtime network arrays (used by net_stack.c instead of NET_IP_A macros) */
extern uint8_t net_cfg_ip[4];
extern uint8_t net_cfg_gw[4];
extern uint8_t net_cfg_mask[4];

#endif

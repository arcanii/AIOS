#ifndef AIOS_LOG_H
#define AIOS_LOG_H
/*
 * AIOS Log — ring buffer + serial echo
 *
 * Root-side threads call aios_log() directly (zero IPC overhead).
 * /proc/log exposes the ring buffer for dmesg.
 *
 * Usage in root-side code:
 *   #define LOG_MODULE "fs"
 *   #define LOG_LEVEL  LOG_LEVEL_DEBUG
 *   #include "aios/aios_log.h"
 *
 *   LOG_INFO("mounted ext2");
 *   LOG_DEBUG_V("sectors read: ", count);
 */

#define LOG_LEVEL_NONE  0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_INFO  3
#define LOG_LEVEL_DEBUG 4

#ifndef LOG_MODULE
#define LOG_MODULE "???"
#endif

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INFO
#endif

#define AIOS_LOG_RING_SIZE 16384  /* 16 KB ring buffer */

/* Core API — implemented in aios_log.c */
void aios_log_init(void);
void aios_log_write(int level, const char *module, const char *msg);
void aios_log_write_v(int level, const char *module, const char *msg,
                   unsigned long val);
int  aios_log_read(char *buf, int bufsize);  /* returns bytes copied */
int  aios_log_len(void);
void aios_log_file_init(void);   /* call after /var/log mounted */                     /* current ring content length */

/* Convenience macros — compile-time level filtering */
#define _AIOS_LOG(lvl, tag, msg) do { \
    if (LOG_LEVEL >= (lvl)) aios_log_write((lvl), LOG_MODULE, (msg)); \
} while (0)

#define _AIOS_LOG_V(lvl, tag, msg, val) do { \
    if (LOG_LEVEL >= (lvl)) aios_log_write_v((lvl), LOG_MODULE, (msg), (val)); \
} while (0)

#define AIOS_LOG_ERROR(msg)       _AIOS_LOG(LOG_LEVEL_ERROR, "ERR", msg)
#define AIOS_LOG_WARN(msg)        _AIOS_LOG(LOG_LEVEL_WARN,  "WRN", msg)
#define AIOS_LOG_INFO(msg)        _AIOS_LOG(LOG_LEVEL_INFO,  "INF", msg)
#define AIOS_LOG_DEBUG(msg)       _AIOS_LOG(LOG_LEVEL_DEBUG, "DBG", msg)

#define AIOS_LOG_ERROR_V(msg, v)  _AIOS_LOG_V(LOG_LEVEL_ERROR, "ERR", msg, (unsigned long)(v))
#define AIOS_LOG_WARN_V(msg, v)   _AIOS_LOG_V(LOG_LEVEL_WARN,  "WRN", msg, (unsigned long)(v))
#define AIOS_LOG_INFO_V(msg, v)   _AIOS_LOG_V(LOG_LEVEL_INFO,  "INF", msg, (unsigned long)(v))
#define AIOS_LOG_DEBUG_V(msg, v)  _AIOS_LOG_V(LOG_LEVEL_DEBUG, "DBG", msg, (unsigned long)(v))

#endif

#pragma once
/* AIOS Logging Infrastructure
 * Per-module log levels with timestamped output
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

/* These must be provided by each PD that uses logging */
extern void _log_puts(const char *s);
extern void _log_put_dec(unsigned long n);
extern void _log_flush(void);
extern unsigned long _log_get_time(void);

static inline void _log_timestamp(void) {
    unsigned long t = _log_get_time();
    unsigned long secs = t % 60;
    unsigned long mins = (t / 60) % 60;
    unsigned long hrs  = (t / 3600) % 24;
    char buf[9];
    buf[0] = '0' + (hrs / 10);
    buf[1] = '0' + (hrs % 10);
    buf[2] = ':';
    buf[3] = '0' + (mins / 10);
    buf[4] = '0' + (mins % 10);
    buf[5] = ':';
    buf[6] = '0' + (secs / 10);
    buf[7] = '0' + (secs % 10);
    buf[8] = '\0';
    _log_puts("[");
    _log_puts(buf);
    _log_puts("] ");
}

#define _LOG(level, tag, msg) do { \
    if (LOG_LEVEL >= level) { \
        _log_timestamp(); \
        _log_puts(tag " " LOG_MODULE ": "); \
        _log_puts(msg); \
        _log_puts("\n"); \
        _log_flush(); \
    } \
} while (0)

#define _LOG_FMT(level, tag, msg, val) do { \
    if (LOG_LEVEL >= level) { \
        _log_timestamp(); \
        _log_puts(tag " " LOG_MODULE ": "); \
        _log_puts(msg); \
        _log_put_dec(val); \
        _log_puts("\n"); \
        _log_flush(); \
    } \
} while (0)

#define LOG_ERROR(msg)       _LOG(LOG_LEVEL_ERROR, "ERR", msg)
#define LOG_WARN(msg)        _LOG(LOG_LEVEL_WARN,  "WRN", msg)
#define LOG_INFO(msg)        _LOG(LOG_LEVEL_INFO,  "INF", msg)
#define LOG_DEBUG(msg)       _LOG(LOG_LEVEL_DEBUG, "DBG", msg)

#define LOG_ERROR_V(msg, v)  _LOG_FMT(LOG_LEVEL_ERROR, "ERR", msg, v)
#define LOG_WARN_V(msg, v)   _LOG_FMT(LOG_LEVEL_WARN,  "WRN", msg, v)
#define LOG_INFO_V(msg, v)   _LOG_FMT(LOG_LEVEL_INFO,  "INF", msg, v)
#define LOG_DEBUG_V(msg, v)  _LOG_FMT(LOG_LEVEL_DEBUG, "DBG", msg, v)

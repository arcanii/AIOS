#pragma once
/*
 * AIOS IPC-based Logging Backend
 *
 * Each PD includes this instead of implementing _log_puts manually.
 * Log messages are written to the PD's shared memory at LOG_MSG_OFF,
 * then the PD notifies the orchestrator with CMD_LOG.
 *
 * Requirements:
 *   - PD must define LOG_IPC_BASE (pointer to shared memory)
 *   - PD must define LOG_IPC_CH   (channel ID to orchestrator)
 *   - PD must include <microkit.h> and "aios/ipc.h"
 */
#ifndef LOG_IPC_BASE
#error "Define LOG_IPC_BASE before including log_ipc.h"
#endif
#ifndef LOG_IPC_CH
#error "Define LOG_IPC_CH before including log_ipc.h"
#endif

static char _log_ipc_buf[LOG_MSG_MAX];
static int  _log_ipc_pos = 0;

void _log_puts(const char *s) {
    while (*s && _log_ipc_pos < LOG_MSG_MAX - 1) {
        _log_ipc_buf[_log_ipc_pos++] = *s++;
    }
}

void _log_put_dec(unsigned long n) {
    char tmp[20]; int i = 0;
    if (n == 0) { _log_puts("0"); return; }
    while (n) { tmp[i++] = '0' + (n % 10); n /= 10; }
    while (i-- > 0 && _log_ipc_pos < LOG_MSG_MAX - 1) {
        _log_ipc_buf[_log_ipc_pos++] = tmp[i];
    }
}

void _log_flush(void) {
    if (_log_ipc_pos == 0) return;
    _log_ipc_buf[_log_ipc_pos] = '\0';

    /* Copy message to shared memory */
    volatile char *dst = (volatile char *)((uintptr_t)LOG_IPC_BASE + LOG_MSG_OFF);
    for (int i = 0; i <= _log_ipc_pos; i++) dst[i] = _log_ipc_buf[i];

    /* Set command */
    *(volatile uint32_t *)((uintptr_t)LOG_IPC_BASE + 0x000) = CMD_LOG;

    /* Notify orchestrator */
    microkit_notify(LOG_IPC_CH);

    _log_ipc_pos = 0;
}

unsigned long _log_get_time(void) {
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    return (unsigned long)(cnt / freq);
}

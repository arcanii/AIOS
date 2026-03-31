/*
 * AIOS Echo Server
 *
 * Placeholder protection domain. Reserved for future use
 * (e.g., network echo, IPC testing, health monitoring).
 *
 * Copyright (c) 2025 AIOS Project
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <microkit.h>
#include "aios/channels.h"
#include "aios/ipc.h"

#define LOG_MODULE "ECHO"
#define LOG_LEVEL  LOG_LEVEL_INFO

/* IPC logging — messages routed through orchestrator */
uintptr_t echo_io;
#define LOG_IPC_BASE echo_io
#define LOG_IPC_CH   CH_ECHO
#include "aios/log_ipc.h"
#include "aios/log.h"

void init(void) {
    /* silent during init — orchestrator reports status */
}

void notified(microkit_channel ch) {
    (void)ch;
}

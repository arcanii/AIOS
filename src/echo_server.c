/*
 * AIOS Echo Server (stub)
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

void init(void) {
    microkit_dbg_puts("ECHO: ready\n");
}

void notified(microkit_channel ch) {
    (void)ch;
}


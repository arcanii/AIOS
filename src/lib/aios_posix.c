/*
 * AIOS POSIX Shim — routes libc I/O through seL4 IPC
 */
#include "aios_posix.h"
#include <arch_stdio.h>
#include <sel4/sel4.h>
#include <stddef.h>
#include <stdint.h>

static seL4_CPtr ser_ep = 0;
static seL4_CPtr fs_ep_cap = 0;

seL4_CPtr aios_get_serial_ep(void) { return ser_ep; }
seL4_CPtr aios_get_fs_ep(void) { return fs_ep_cap; }

static size_t aios_stdio_write(void *data, size_t count)
{
    if (!ser_ep) return count;
    char *buf = (char *)data;
    for (size_t i = 0; i < count; i++) {
        seL4_SetMR(0, (seL4_Word)(uint8_t)buf[i]);
        seL4_Call(ser_ep, seL4_MessageInfo_new(AIOS_SER_PUTC, 0, 0, 1));
    }
    return count;
}

int aios_getchar(void)
{
    if (!ser_ep) return -1;
    seL4_MessageInfo_t reply = seL4_Call(ser_ep,
        seL4_MessageInfo_new(AIOS_SER_GETC, 0, 0, 0));
    return (int)(long)seL4_GetMR(0);
}

void aios_init(seL4_CPtr serial_ep, seL4_CPtr fs_endpoint)
{
    ser_ep = serial_ep;
    fs_ep_cap = fs_endpoint;
    sel4muslcsys_register_stdio_write_fn(aios_stdio_write);
}

"""Patch orch_main.inc: read /etc/hostname and /etc/motd at boot."""

from .utils import (set_module, log, read_file, write_file, replace_block)

PATH = "src/orch/orch_main.inc"

MOTD_HOSTNAME_CODE = (
    '    /* Read /etc/hostname */\n'
    '    st = fs_open_sync("/etc/hostname");\n'
    '    if (st == 0) {\n'
    '        uint32_t hfd = RD32(fs_data, FS_FD);\n'
    '        uint32_t hsz = RD32(fs_data, FS_FILESIZE);\n'
    '        if (hsz > 63) hsz = 63;\n'
    '        fs_read_sync(hfd, 0, hsz);\n'
    '        volatile char *hdata = (volatile char *)(fs_data + FS_DATA);\n'
    '        int hi = 0;\n'
    '        while (hi < (int)hsz && hdata[hi] && hdata[hi] != \'\\n\') {\n'
    '            hostname[hi] = hdata[hi]; hi++;\n'
    '        }\n'
    '        hostname[hi] = \'\\0\';\n'
    '        fs_close_sync(hfd);\n'
    '    }\n'
    '\n'
    '    /* Read and display /etc/motd */\n'
    '    st = fs_open_sync("/etc/motd");\n'
    '    if (st == 0) {\n'
    '        uint32_t mfd = RD32(fs_data, FS_FD);\n'
    '        uint32_t msz = RD32(fs_data, FS_FILESIZE);\n'
    '        if (msz > 2048) msz = 2048;\n'
    '        fs_read_sync(mfd, 0, msz);\n'
    '        uint32_t mgot = RD32(fs_data, FS_LENGTH);\n'
    '        volatile char *mdata = (volatile char *)(fs_data + FS_DATA);\n'
    '        for (uint32_t mi = 0; mi < mgot; mi++) ser_putc(mdata[mi]);\n'
    '        fs_close_sync(mfd);\n'
    '    }\n'
    '    ser_flush();\n'
    '\n'
    '    LOG_INFO("system ready, awaiting login");\n'
    '    ser_puts("\\nLogin: ");'
)

def run():
    set_module("MAIN")
    log("=== Patching init() for /etc/hostname + /etc/motd ===")
    src = read_file(PATH)
    ok = True

    if '/etc/motd' not in src:
        old = '    LOG_INFO("system ready, awaiting login");\n    ser_puts("\\nLogin: ");'
        src, s = replace_block(src, old, MOTD_HOSTNAME_CODE, "/etc/hostname + /etc/motd in init()")
        ok = ok and s
    else:
        log("/etc/motd already in init(), skipping")

    write_file(PATH, src)
    return ok

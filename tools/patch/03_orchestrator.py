"""Patch orchestrator.c and orch_auth_cmd.inc: session globals, shell/home reading."""

from .utils import (set_module, log, read_file, write_file,
                    replace_block, insert_before)

def run():
    set_module("ORCH")
    log("=== Patching orchestrator globals + auth reading ===")
    ok = True

    # --- 3a. Add AUTH IPC offsets to orch_auth_cmd.inc ---
    path = "src/orch/orch_auth_cmd.inc"
    src = read_file(path)

    if 'AUTH_HOME' not in src:
        src, s = insert_before(src,
            '/* ── Auth server helpers (PPC via CH_AUTH) ──────────── */',
            '/* ── Auth IPC layout (extended) ────────────────────── */\n'
            '#define AUTH_HOME       0x140   /* 64 bytes */\n'
            '#define AUTH_SHELL      0x180   /* 64 bytes */\n'
            '#define AUTH_GECOS      0x1C0   /* 64 bytes */\n'
            '\n',
            "AUTH IPC offsets in orch_auth_cmd.inc")
        ok = ok and s
    else:
        log("AUTH_HOME already in orch_auth_cmd.inc, skipping")

    # --- 3b. Read shell/home after login ---
    if 'current_shell' not in src:
        old = '        current_is_root = (current_uid == 0) ? 1 : 0;'
        new = (
            '        current_is_root = (current_uid == 0) ? 1 : 0;\n'
            '        /* Read shell and home from auth response */\n'
            '        {\n'
            '            volatile char *sh = (volatile char *)(auth_io + AUTH_SHELL);\n'
            '            volatile char *hm = (volatile char *)(auth_io + AUTH_HOME);\n'
            '            int k;\n'
            '            k = 0; while (sh[k] && k < 63) { current_shell[k] = sh[k]; k++; } current_shell[k] = \'\\0\';\n'
            '            k = 0; while (hm[k] && k < 63) { current_home[k] = hm[k]; k++; } current_home[k] = \'\\0\';\n'
            '        }'
        )
        src, s = replace_block(src, old, new, "shell/home reading in auth_login_sync")
        ok = ok and s
    else:
        log("Shell reading already in orch_auth_cmd.inc, skipping")

    # --- 3c. Clear shell/home on logout ---
    if "current_shell[0] = '\\0'" not in src:
        old_logout = "    current_username[0] = '\\0';\n}"
        # Only replace the one inside auth_logout_sync
        logout_idx = src.find('static void auth_logout_sync(void)')
        if logout_idx >= 0:
            end_idx = src.find(old_logout, logout_idx)
            if end_idx >= 0:
                new_end = (
                    "    current_username[0] = '\\0';\n"
                    "    current_shell[0] = '\\0';\n"
                    "    current_home[0] = '\\0';\n"
                    "}"
                )
                src = src[:end_idx] + new_end + src[end_idx + len(old_logout):]
                log("Added shell/home clearing to auth_logout_sync()")
            else:
                log("WARNING: Could not find end of auth_logout_sync()")
                ok = False
        else:
            log("WARNING: Could not find auth_logout_sync()")
            ok = False
    else:
        log("Logout already clears shell/home, skipping")

    write_file(path, src)

    # --- 3d. Add session globals to orchestrator.c ---
    path2 = "src/orchestrator.c"
    src2 = read_file(path2)

    if 'current_shell' not in src2:
        src2, s = replace_block(src2,
            'static uint32_t current_gid = 0;',
            'static uint32_t current_gid = 0;\n'
            'static char hostname[64] = "aios";\n'
            'static char current_shell[64] = "/bin/osh";\n'
            'static char current_home[64] = "/root";\n'
            'static int session_shell_slot = -1;  /* slot running login shell, -1 if none */',
            "session globals in orchestrator.c")
        ok = ok and s
    else:
        log("Session globals already in orchestrator.c, skipping")

    write_file(path2, src2)
    return ok

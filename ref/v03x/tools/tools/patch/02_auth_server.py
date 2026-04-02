"""Patch auth_server.c: 7-field /etc/passwd, shell/home/gecos support."""

from .utils import (set_module, log, read_file, write_file,
                    replace_block, insert_after, replace_function)

PATH = "src/auth_server.c"

NEW_LOAD_PASSWD = r'''void handle_load_passwd(void) {
    volatile char *data = (volatile char *)(auth_io + AUTH_DATA);
    char buf[3584];
    int i = 0;
    while (data[i] && i < 3583) { buf[i] = data[i]; i++; }
    buf[i] = '\0';

    /* Reset user database */
    my_memset(users, 0, sizeof(users));
    num_users = 0;

    /* Parse line by line: username:hash:uid:gid[:gecos:home:shell] */
    int pos = 0;
    while (buf[pos] && num_users < MAX_USERS) {
        /* Skip blank lines and comments */
        if (buf[pos] == '\n') { pos++; continue; }
        if (buf[pos] == '#') {
            while (buf[pos] && buf[pos] != '\n') pos++;
            if (buf[pos] == '\n') pos++;
            continue;
        }

        user_entry_t *u = &users[num_users];
        int start, flen;

        /* Field 1: username */
        start = pos;
        while (buf[pos] && buf[pos] != ':' && buf[pos] != '\n') pos++;
        if (buf[pos] != ':') goto next_line;
        flen = pos - start; if (flen > 31) flen = 31;
        for (int j = 0; j < flen; j++) u->username[j] = buf[start + j];
        u->username[flen] = '\0';
        pos++;

        /* Field 2: password hash */
        start = pos;
        while (buf[pos] && buf[pos] != ':' && buf[pos] != '\n') pos++;
        if (buf[pos] != ':') goto next_line;
        flen = pos - start; if (flen > 64) flen = 64;
        for (int j = 0; j < flen; j++) u->passhash[j] = buf[start + j];
        u->passhash[flen] = '\0';
        pos++;

        /* Field 3: uid */
        u->uid = (uint32_t)parse_uint(buf, &pos);
        if (buf[pos] == ':') pos++; else goto next_line;

        /* Field 4: gid */
        u->gid = (uint32_t)parse_uint(buf, &pos);

        /* Fields 5-7 are optional (backwards compat with 4-field format) */
        if (buf[pos] == ':') {
            pos++;
            /* Field 5: gecos */
            start = pos;
            while (buf[pos] && buf[pos] != ':' && buf[pos] != '\n') pos++;
            flen = pos - start; if (flen > 63) flen = 63;
            for (int j = 0; j < flen; j++) u->gecos[j] = buf[start + j];
            u->gecos[flen] = '\0';

            if (buf[pos] == ':') {
                pos++;
                /* Field 6: home */
                start = pos;
                while (buf[pos] && buf[pos] != ':' && buf[pos] != '\n') pos++;
                flen = pos - start; if (flen > 63) flen = 63;
                for (int j = 0; j < flen; j++) u->home[j] = buf[start + j];
                u->home[flen] = '\0';

                if (buf[pos] == ':') {
                    pos++;
                    /* Field 7: shell */
                    start = pos;
                    while (buf[pos] && buf[pos] != ':' && buf[pos] != '\n') pos++;
                    flen = pos - start; if (flen > 63) flen = 63;
                    for (int j = 0; j < flen; j++) u->shell[j] = buf[start + j];
                    u->shell[flen] = '\0';
                }
            }
        }

        /* Defaults for missing fields */
        if (u->home[0] == '\0') {
            if (u->uid == 0) {
                my_strcpy(u->home, "/root");
            } else {
                my_strcpy(u->home, "/home/");
                int hlen = 6;
                for (int j = 0; u->username[j] && hlen < 63; j++)
                    u->home[hlen++] = u->username[j];
                u->home[hlen] = '\0';
            }
        }
        if (u->shell[0] == '\0') {
            my_strcpy(u->shell, "/bin/osh");
        }

        u->active = 1;
        u->is_root = (u->uid == 0) ? 1 : 0;
        num_users++;

next_line:
        while (buf[pos] && buf[pos] != '\n') pos++;
        if (buf[pos] == '\n') pos++;
    }

    microkit_dbg_puts("AUTH: loaded ");
    char nbuf[8];
    int ni = 0;
    int tmp = num_users;
    if (tmp == 0) { nbuf[ni++] = '0'; }
    else { while (tmp > 0) { nbuf[ni++] = '0' + (tmp % 10); tmp /= 10; } }
    for (int r = ni - 1; r >= 0; r--) microkit_dbg_putc(nbuf[r]);
    microkit_dbg_puts(" users from /etc/passwd\n");

    WR32(auth_io, AUTH_STATUS, 0);
}'''

LOGIN_RESPONSE_INJECT = (
    '                    WR32(auth_io, AUTH_SESSION, sessions[si].token);\n'
    '                    /* Write home/shell/gecos to shared memory */\n'
    '                    {\n'
    '                        volatile char *hp = (volatile char *)(auth_io + AUTH_HOME);\n'
    '                        volatile char *sp = (volatile char *)(auth_io + AUTH_SHELL);\n'
    '                        volatile char *gp = (volatile char *)(auth_io + AUTH_GECOS);\n'
    '                        int k;\n'
    '                        k = 0; while (users[ui].home[k] && k < 63) { hp[k] = users[ui].home[k]; k++; } hp[k] = \'\\0\';\n'
    '                        k = 0; while (users[ui].shell[k] && k < 63) { sp[k] = users[ui].shell[k]; k++; } sp[k] = \'\\0\';\n'
    '                        k = 0; while (users[ui].gecos[k] && k < 63) { gp[k] = users[ui].gecos[k]; k++; } gp[k] = \'\\0\';\n'
    '                    }\n'
    '                    return;'
)

def run():
    set_module("AUTH")
    log("=== Patching src/auth_server.c ===")
    src = read_file(PATH)
    ok = True

    # 2a. Add IPC offsets
    if 'AUTH_HOME' not in src:
        src, s = replace_block(src,
            '#define AUTH_DATA       0x200',
            '#define AUTH_HOME       0x140   /* 64 bytes: user home directory */\n'
            '#define AUTH_SHELL      0x180   /* 64 bytes: user login shell */\n'
            '#define AUTH_GECOS      0x1C0   /* 64 bytes: user full name */\n'
            '\n'
            '#define AUTH_DATA       0x200',
            "AUTH IPC offsets")
        ok = ok and s
    else:
        log("AUTH_HOME already present, skipping")

    # 2b. Add fields to user_entry_t
    if 'char   shell[64]' not in src:
        src, s = replace_block(src,
            '    int    is_root;\n} user_entry_t;',
            '    int    is_root;\n'
            '    char   home[64];      /* home directory, e.g. /root */\n'
            '    char   shell[64];     /* login shell, e.g. /bin/osh */\n'
            '    char   gecos[64];     /* full name / comment */\n'
            '} user_entry_t;',
            "user_entry_t fields")
        ok = ok and s
    else:
        log("user_entry_t already has shell, skipping")

    # 2c. Default users — root
    if 'users[0].home' not in src:
        src, s = replace_block(src,
            '    users[0].is_root = 1;\n\n    /* user:user */',
            '    users[0].is_root = 1;\n'
            '    my_strcpy(users[0].home, "/root");\n'
            '    my_strcpy(users[0].shell, "/bin/osh");\n'
            '    my_strcpy(users[0].gecos, "System Administrator");\n'
            '\n    /* user:user */',
            "root user defaults")
        ok = ok and s
    else:
        log("Root defaults already present, skipping")

    # 2c. Default users — user
    if 'users[1].home' not in src:
        src, s = replace_block(src,
            '    users[1].is_root = 0;\n\n    num_users = 2;',
            '    users[1].is_root = 0;\n'
            '    my_strcpy(users[1].home, "/home/user");\n'
            '    my_strcpy(users[1].shell, "/bin/shell.bin");\n'
            '    my_strcpy(users[1].gecos, "Regular User");\n'
            '\n    num_users = 2;',
            "regular user defaults")
        ok = ok and s
    else:
        log("User defaults already present, skipping")

    # 2d. Replace handle_load_passwd()
    if 'Field 5: gecos' not in src:
        src, s = replace_function(src,
            'void handle_load_passwd(void) {',
            NEW_LOAD_PASSWD,
            "handle_load_passwd()")
        ok = ok and s
    else:
        log("handle_load_passwd() already updated, skipping")

    # 2e. Add shell/home to login response
    login_marker = 'WR32(auth_io, AUTH_SESSION, sessions[si].token);'
    if login_marker in src and 'auth_io + AUTH_HOME' not in src:
        idx = src.find(login_marker)
        # Find the "return;" after this
        ret_str = '                    return;'
        ret_idx = src.find(ret_str, idx)
        if ret_idx >= 0:
            old = src[idx:ret_idx + len(ret_str)]
            src = src[:idx] + LOGIN_RESPONSE_INJECT + src[ret_idx + len(ret_str):]
            log("Added shell/home/gecos to login response")
        else:
            log("WARNING: Could not find return after login marker")
            ok = False
    elif 'auth_io + AUTH_HOME' in src:
        log("Login response already has AUTH_HOME, skipping")

    write_file(PATH, src)
    return ok

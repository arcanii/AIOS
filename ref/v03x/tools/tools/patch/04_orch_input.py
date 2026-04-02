"""Patch orch_input.inc: auto-launch shell after login, respawn on exit, hostname prompt."""

from .utils import (set_module, log, read_file, write_file, replace_block)

PATH = "src/orch/orch_input.inc"

def run():
    set_module("INPUT")
    log("=== Patching login auto-launch + respawn + prompt ===")
    src = read_file(PATH)
    ok = True

    # --- 4a. Auto-launch shell after login ---
    if 'auto-launch' not in src:
        old = (
            '                if (rc == 0) {\n'
            '                    auth_state = AUTH_AUTHENTICATED;\n'
            '                    ser_puts("\\nWelcome, ");\n'
            '                    ser_puts(current_username);\n'
            '                    if (current_is_root) ser_puts(" (root)");\n'
            '                    ser_puts("\\n\\n");\n'
            '                    print_prompt();'
        )
        new = (
            '                if (rc == 0) {\n'
            '                    auth_state = AUTH_AUTHENTICATED;\n'
            '                    ser_puts("\\nWelcome, ");\n'
            '                    ser_puts(current_username);\n'
            '                    if (current_is_root) ser_puts(" (root)");\n'
            '                    ser_puts("\\n\\n");\n'
            '                    /* auto-launch user shell from /etc/passwd */\n'
            '                    if (current_shell[0] != \'\\0\' &&\n'
            '                        my_strcmp(current_shell, "/bin/osh") != 0) {\n'
            '                        /* Launch external shell program */\n'
            '                        cmd_exec(current_shell);\n'
            '                        /* Track which slot is the login shell */\n'
            '                        for (int ss = 0; ss < NUM_SANDBOXES; ss++) {\n'
            '                            if (slot_in_use(ss) && slot_proc(ss)->foreground) {\n'
            '                                session_shell_slot = ss;\n'
            '                                break;\n'
            '                            }\n'
            '                        }\n'
            '                    } else {\n'
            '                        /* Built-in orchestrator shell */\n'
            '                        print_prompt();\n'
            '                    }'
        )
        src, s = replace_block(src, old, new, "login auto-launch")
        ok = ok and s
    else:
        log("Auto-launch already present, skipping")

    # --- 4b. Session respawn on shell exit ---
    # Use specific guard: look for the actual respawn comment, not just variable name
    if 'Login shell exited' not in src:
        old = (
            '    if (slot_proc(slot)->foreground) {\n'
            '        orch_state = RUNNING;\n'
            '        print_prompt();\n'
            '    }'
        )
        new = (
            '    if (slot_proc(slot)->foreground) {\n'
            '        if (slot == session_shell_slot) {\n'
            '            /* Login shell exited — respawn login prompt */\n'
            '            session_shell_slot = -1;\n'
            '            auth_logout_sync();\n'
            '            auth_state = AUTH_LOGIN_USER;\n'
            '            ser_puts("\\n\\nLogin: ");\n'
            '            ser_flush();\n'
            '        } else {\n'
            '            orch_state = RUNNING;\n'
            '            print_prompt();\n'
            '        }\n'
            '    }'
        )
        src, s = replace_block(src, old, new, "session respawn in handle_exec_done")
        ok = ok and s
    else:
        log("Session respawn already present, skipping")

    # --- 4c. Update prompt to use hostname ---
    if 'ser_puts(hostname)' not in src:
        old = (
            'static void print_prompt(void) {\n'
            '    ser_puts(current_username);\n'
            '    if (current_is_root)\n'
            '        ser_puts("@aios# ");\n'
            '    else\n'
            '        ser_puts("@aios$ ");\n'
            '    ser_flush();\n'
            '}'
        )
        new = (
            'static void print_prompt(void) {\n'
            '    ser_puts(current_username);\n'
            '    ser_puts("@");\n'
            '    ser_puts(hostname);\n'
            '    if (current_is_root)\n'
            '        ser_puts("# ");\n'
            '    else\n'
            '        ser_puts("$ ");\n'
            '    ser_flush();\n'
            '}'
        )
        src, s = replace_block(src, old, new, "print_prompt hostname")
        ok = ok and s
    else:
        log("Prompt already uses hostname, skipping")

    write_file(PATH, src)
    return ok

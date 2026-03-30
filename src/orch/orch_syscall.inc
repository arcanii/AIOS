/* Sandbox syscall handler
 * Auto-extracted from orchestrator.c
 * Lines 1799-2425
 */

/* ── Protected procedure call handler (sandbox syscalls) */
microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    int slot = ch_to_slot(ch);
    if (slot < 0) {
        seL4_SetMR(0, -1);
        return microkit_msginfo_new(0, 1);
    }
    uintptr_t sio = sbx_io[slot];
    uintptr_t scode = sbx_code[slot];

    uint64_t syscall_nr = seL4_GetMR(0);
    int64_t result = -1;

    switch (syscall_nr) {
    case SYS_OPEN: {
        volatile char *path = (volatile char *)(sio + 0x200);
        uint32_t flags = (uint32_t)seL4_GetMR(1);
        char raw_name[256];
        int i = 0;
        while (path[i] && i < 255) { raw_name[i] = path[i]; i++; }
        raw_name[i] = '\0';
        char fname[256];
        resolve_path(raw_name, fname, sizeof(fname));
        /* Permission check */
        uint32_t open_mode = ACCESS_READ;
        if (flags & (0x0001 | 0x0002)) open_mode |= ACCESS_WRITE;
        if (auth_check_file_sync(fname, open_mode) != 0) {
            result = -13; /* EACCES */
            break;
        }
        fs_set_creator_sync(current_uid, current_gid);
        int st = fs_open_sync(fname);
        if (st != 0 && (flags & 0x0040)) {
            /* O_CREAT: file not found, create it */
            st = fs_create_sync(fname);
            /* create already opens the file — use its fd directly */
        }
        if (st == 0) {
            result = RD32(fs_data, FS_FD);
            seL4_SetMR(1, RD32(fs_data, FS_FILESIZE));
        } else {
            result = -1;
        }
        break;
    }
    case SYS_READ: {

        /* Check if this is a pipe fd */
        {
            int rfd = (int)seL4_GetMR(1);
            if (rfd >= PIPE_FD_BASE && rfd < PIPE_FD_BASE + MAX_PIPE_FDS) {
                int pi_idx = rfd - PIPE_FD_BASE;
                if (pi_idx >= 0 && pi_idx < MAX_PIPE_FDS && pipe_fds[slot][pi_idx].active) {
                    int pidx = pipe_fds[slot][pi_idx].pipe_idx;
                    pipe_t *pp = &pipes[pidx];
                    uint32_t avail = pp->write_pos - pp->read_pos;
                    if (avail == 0 && pp->closed_write) {
                        result = 0;  /* EOF */
                    } else if (avail > 0) {
                        uint32_t req = (uint32_t)seL4_GetMR(3);
                        if (req > avail) req = avail;
                        volatile uint8_t *dst = (volatile uint8_t *)(sio + 0x400);
                        for (uint32_t bi = 0; bi < req; bi++)
                            dst[bi] = pp->buf[(pp->read_pos + bi) % PIPE_BUF_SIZE];
                        pp->read_pos += req;
                        result = (int64_t)req;
                    } else {
                        result = 0;  /* would block — return 0 for now */
                    }
                    break;
                }
            }
        }
        uint32_t fd     = (uint32_t)seL4_GetMR(1);
        uint32_t offset = (uint32_t)seL4_GetMR(2);
        uint32_t len    = (uint32_t)seL4_GetMR(3);
        int st = fs_read_sync(fd, offset, len);
        if (st == 0 || st == 3) {
            uint32_t got = RD32(fs_data, FS_LENGTH);
            /* Copy data to sandbox_io+0x400 */
            volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
            volatile uint8_t *dst = (volatile uint8_t *)(sio + 0x400);
            for (uint32_t i = 0; i < got; i++) dst[i] = src[i];
            result = got;
        }
        break;
    }
    case SYS_WRITE: {

        /* Check if this is a pipe fd */
        {
            int wfd = (int)seL4_GetMR(1);
            if (wfd >= PIPE_FD_BASE && wfd < PIPE_FD_BASE + MAX_PIPE_FDS) {
                int pi_idx = wfd - PIPE_FD_BASE;
                if (pi_idx >= 0 && pi_idx < MAX_PIPE_FDS && pipe_fds[slot][pi_idx].active) {
                    int pidx = pipe_fds[slot][pi_idx].pipe_idx;
                    pipe_t *pp = &pipes[pidx];
                    if (pp->closed_read) { result = -1; break; }  /* broken pipe */
                    volatile uint8_t *src = (volatile uint8_t *)(sio + 0x400);
                    uint32_t len = (uint32_t)seL4_GetMR(2);
                    uint32_t space = PIPE_BUF_SIZE - (pp->write_pos - pp->read_pos);
                    if (len > space) len = space;
                    for (uint32_t bi = 0; bi < len; bi++)
                        pp->buf[(pp->write_pos + bi) % PIPE_BUF_SIZE] = src[bi];
                    pp->write_pos += len;
                    result = (int64_t)len;
                    break;
                }
            }
        }
        uint32_t fd  = (uint32_t)seL4_GetMR(1);
        uint32_t len = (uint32_t)seL4_GetMR(2);
        /* Data is at sandbox_io+0x400 */
        volatile uint8_t *src = (volatile uint8_t *)(sio + 0x400);
        volatile uint8_t *dst = (volatile uint8_t *)(fs_data + FS_DATA);
        for (uint32_t i = 0; i < len && i < FS_DATA_MAX; i++) dst[i] = src[i];
        int st = fs_write_sync(fd, len);
        if (st == 0) result = RD32(fs_data, FS_LENGTH);
        break;
    }
    case SYS_CLOSE: {

        /* Check if this is a pipe fd */
        {
            int cfd = (int)seL4_GetMR(1);
            if (cfd >= PIPE_FD_BASE && cfd < PIPE_FD_BASE + MAX_PIPE_FDS) {
                int pi_idx = cfd - PIPE_FD_BASE;
                if (pi_idx >= 0 && pi_idx < MAX_PIPE_FDS && pipe_fds[slot][pi_idx].active) {
                    int pidx = pipe_fds[slot][pi_idx].pipe_idx;
                    if (pipe_fds[slot][pi_idx].is_write)
                        pipes[pidx].closed_write = 1;
                    else
                        pipes[pidx].closed_read = 1;
                    pipe_fds[slot][pi_idx].active = 0;
                    /* Free pipe if both ends closed */
                    if (pipes[pidx].closed_read && pipes[pidx].closed_write)
                        pipes[pidx].active = 0;
                    result = 0;
                    break;
                }
            }
        }
        uint32_t fd = (uint32_t)seL4_GetMR(1);
        fs_close_sync(fd);
        result = 0;
        break;
    }
    case SYS_UNLINK: {
        volatile char *path = (volatile char *)(sio + 0x200);
        char raw[256];
        int i = 0;
        while (path[i] && i < 255) { raw[i] = path[i]; i++; }
        raw[i] = '\0';
        char fname[256];
        resolve_path(raw, fname, sizeof(fname));
        /* Permission check */
        if (auth_check_file_sync(fname, ACCESS_WRITE) != 0) {
            result = -13; /* EACCES */
            break;
        }
        int st = fs_delete_sync(fname);
        result = (st == 0) ? 0 : -1;
        break;
    }
    case SYS_READDIR: {
        int st = fs_list_sync(cwd);
        if (st == 0) {
            uint32_t count = RD32(fs_data, FS_LENGTH);
            uint32_t total_bytes = RD32(fs_data, FS_FILESIZE);
            /* Copy variable-length listing to sandbox_io+0x400 (max 0xC00) */
            volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
            volatile uint8_t *dst = (volatile uint8_t *)(sio + 0x400);
            if (total_bytes > 0xC00) total_bytes = 0xC00;
            for (uint32_t i = 0; i < total_bytes; i++) dst[i] = src[i];
            seL4_SetMR(1, total_bytes);
            result = count;
        }
        break;
    }
    case SYS_PUTC: {
        char c = (char)seL4_GetMR(1);
        ser_putc(c);
        ser_flush();
        result = 0;
        break;
    }
    case 32: { /* SYS_PUTS_DIRECT */
        volatile char *str = (volatile char *)(sio + 0x200);
        char buf[256];
        int i = 0;
        while (str[i] && i < 255) { buf[i] = str[i]; i++; }
        buf[i] = '\0';
        ser_puts(buf);
        ser_flush();
        result = 0;
        break;
    }
    case SYS_GETC: {
        result = (int64_t)(unsigned char)rx_getc_blocking();
        break;
    }
    case SYS_STAT: {
        volatile char *path = (volatile char *)(sio + 0x200);
        char raw[256];
        int i = 0;
        while (path[i] && i < 255) { raw[i] = path[i]; i++; }
        raw[i] = '\0';
        char fname[256];
        resolve_path(raw, fname, sizeof(fname));
        uint32_t sz = 0, fuid = 0, fgid = 0, fmode = 0, fmtime = 0;
        int st = fs_stat_ex_sync(fname, &sz, &fuid, &fgid, &fmode, &fmtime);
        if (st == 0) {
            result = 0;
            seL4_SetMR(1, sz);
            seL4_SetMR(2, fuid);
            seL4_SetMR(3, fgid);
            seL4_SetMR(4, fmode);
            seL4_SetMR(5, fmtime);
        } else {
            result = -1;
        }
        break;
    }
    case SYS_FSTAT: {
        /* Return file size for open fd */
        result = 0;
        seL4_SetMR(1, seL4_GetMR(1)); /* placeholder */
        break;
    }
    case SYS_LSEEK: {
        /* Handled client-side via fd_pos in sandbox */
        result = 0;
        break;
    }
    case SYS_MKDIR: {
        volatile char *path = (volatile char *)(sio + 0x200);
        char raw[256];
        int i = 0;
        while (path[i] && i < 255) { raw[i] = path[i]; i++; }
        raw[i] = '\0';
        char dname[256];
        resolve_path(raw, dname, sizeof(dname));
        /* Permission check */
        if (auth_check_file_sync(dname, ACCESS_WRITE) != 0) {
            result = -13; /* EACCES */
            break;
        }
        fs_set_creator_sync(current_uid, current_gid);
        result = fs_mkdir_sync(dname);
        break;
    }
    case SYS_RMDIR: {
        volatile char *path = (volatile char *)(sio + 0x200);
        char raw[256];
        int i = 0;
        while (path[i] && i < 255) { raw[i] = path[i]; i++; }
        raw[i] = '\0';
        char dname[256];
        resolve_path(raw, dname, sizeof(dname));
        /* Permission check */
        if (auth_check_file_sync(dname, ACCESS_WRITE) != 0) {
            result = -13; /* EACCES */
            break;
        }
        result = fs_rmdir_sync(dname);
        break;
    }
    case SYS_RENAME: {
        volatile char *p = (volatile char *)(sio + 0x200);
        char raw_old[128], raw_new[128];
        int i = 0;
        while (p[i] && i < 127) { raw_old[i] = p[i]; i++; }
        raw_old[i] = '\0';
        i++;
        int j = 0;
        while (p[i] && j < 127) { raw_new[j] = p[i]; i++; j++; }
        raw_new[j] = '\0';
        char oldname[256], newname[256];
        resolve_path(raw_old, oldname, sizeof(oldname));
        resolve_path(raw_new, newname, sizeof(newname));
        result = fs_rename_sync(oldname, newname);
        break;
    }
    case SYS_GETCWD: {
        volatile char *dst = (volatile char *)(sio + 0x200);
        int ci = 0;
        while (cwd[ci] && ci < 255) { dst[ci] = cwd[ci]; ci++; }
        dst[ci] = '\0';
        result = 0;
        break;
    }
    case SYS_CHDIR: {
        volatile char *path = (volatile char *)(sio + 0x200);
        char dname[256];
        int i = 0;
        while (path[i] && i < 255) { dname[i] = path[i]; i++; }
        dname[i] = '\0';
        if (dname[0] == '/' && dname[1] == '\0') {
            cwd[0] = '/'; cwd[1] = '\0';
            result = 0;
        } else if (dname[0] == '.' && dname[1] == '.' && dname[2] == '\0') {
            /* cd .. : go up one level */
            int len = 0;
            while (cwd[len]) len++;
            if (len > 1) {
                len--;
                while (len > 0 && cwd[len] != '/') len--;
                if (len == 0) len = 1;
                cwd[len] = '\0';
            }
            result = 0;
        } else {
            /* Build absolute path */
            char abspath[256];
            int j = 0;
            if (dname[0] == '/') {
                while (dname[j] && j < 255) { abspath[j] = dname[j]; j++; }
            } else {
                int k = 0;
                while (cwd[k] && j < 254) abspath[j++] = cwd[k++];
                if (j > 1) abspath[j++] = '/';
                k = 0;
                while (dname[k] && j < 255) abspath[j++] = dname[k++];
            }
            abspath[j] = '\0';
            /* Verify directory exists */
            int st = fs_stat_sync(abspath);
            if (st == 0) {
                j = 0;
                while (abspath[j] && j < 255) { cwd[j] = abspath[j]; j++; }
                cwd[j] = '\0';
                result = 0;
            } else {
                result = -1;
            }
        }
        break;
    }
    case SYS_GETPID: {
        result = (int64_t)proctab[slot].pid;
        break;
    }
    case SYS_SLEEP: {
        /* Busy-wait approximation */
        uint32_t secs = (uint32_t)seL4_GetMR(1);
        for (volatile uint32_t s = 0; s < secs; s++)
            for (volatile uint32_t i = 0; i < 1000000; i++);
        result = 0;
        break;
    }
    case SYS_EXEC: {
        /* Chaining exec: load child program into sandbox_code */
        volatile char *path = (volatile char *)(sio + 0x200);
        char fname[256];
        int i = 0;
        while (path[i] && i < 255) { fname[i] = path[i]; i++; }
        fname[i] = '\0';

        /* Open child program */
        int st = fs_open_sync(fname);
        if (st != 0) { result = -1; break; }
        uint32_t fd = RD32(fs_data, FS_FD);
        uint32_t child_size = RD32(fs_data, FS_FILESIZE);
        if (child_size == 0 || child_size > 0x80000) { /* max 512K for child */
            fs_close_sync(fd);
            result = -1;
            break;
        }

        /* Store parent size so sandbox can back it up */
        WR32(sio, SBX_EXEC_PARENT_SIZE, exec_loaded_bytes);

        /* Load child into sandbox_code (overwriting parent) */
        volatile uint8_t *code = (volatile uint8_t *)scode;
        uint32_t offset = 0;
        while (offset < child_size) {
            uint32_t chunk = child_size - offset;
            if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
            st = fs_read_sync(fd, offset, chunk);
            if (st != 0 && st != 3) break;
            uint32_t got = RD32(fs_data, FS_LENGTH);
            if (got == 0) break;
            volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
            for (uint32_t j = 0; j < got; j++) code[offset + j] = src[j];
            offset += got;
        }
        fs_close_sync(fd);

        WR32(sio, SBX_EXEC_CHILD_SIZE, offset);
        exec_loaded_bytes = offset;

        result = SBX_EXEC_MAGIC;
        break;
    }
    case SYS_EXEC_DONE: {
        /* Child finished — sandbox already restored parent code */
        exec_loaded_bytes = RD32(sio, SBX_EXEC_PARENT_SIZE);
        /* Return the child exit code (passed in MR1) */
        result = (int64_t)(int32_t)seL4_GetMR(1);
        break;
    }
    case SYS_EXIT: {

        /* Clean up any pipe fds for this slot */
        for (int pi = 0; pi < MAX_PIPE_FDS; pi++) {
            if (pipe_fds[slot][pi].active) {
                int pidx = pipe_fds[slot][pi].pipe_idx;
                if (pipe_fds[slot][pi].is_write)
                    pipes[pidx].closed_write = 1;
                else
                    pipes[pidx].closed_read = 1;
                pipe_fds[slot][pi].active = 0;
                if (pipes[pidx].closed_read && pipes[pidx].closed_write)
                    pipes[pidx].active = 0;
            }
        }
        result = 0;
        break;
    }

    case SYS_GETUID: {
        result = (int64_t)current_uid;
        break;
    }
    case SYS_GETGID: {
        result = (int64_t)current_gid;
        break;
    }
    case SYS_GETEUID: {
        result = (int64_t)current_uid;  /* no suid support */
        break;
    }
    case SYS_GETEGID: {
        result = (int64_t)current_gid;
        break;
    }
    case SYS_GETPPID: {
        result = 1;  /* orchestrator is always parent */
        break;
    }
    case SYS_ACCESS: {
        volatile char *path = (volatile char *)(sio + 0x200);
        uint32_t amode = (uint32_t)seL4_GetMR(1);
        char fname[256];
        int i = 0;
        while (path[i] && i < 255) { fname[i] = path[i]; i++; }
        fname[i] = '\0';
        /* Convert access mode: F_OK=0, R_OK=4, W_OK=2, X_OK=1 */
        uint32_t check = 0;
        if (amode & 4) check |= ACCESS_READ;
        if (amode & 2) check |= ACCESS_WRITE;
        if (amode == 0) {
            /* F_OK: just check existence */
            int st = fs_stat_sync(fname);
            result = (st == 0) ? 0 : -1;
            break;
        }
        result = auth_check_file_sync(fname, check);
        break;
    }
    case SYS_UMASK: {
        uint32_t new_mask = (uint32_t)seL4_GetMR(1);
        result = (int64_t)current_umask;
        current_umask = new_mask & 0777;
        break;
    }
    case SYS_DUP: {
        int oldfd = (int)seL4_GetMR(1);
        /* Find lowest free fd in sandbox fd table */
        int newfd = -1;
        for (int i = 0; i < MAX_PIPE_FDS; i++) {
            if (!pipe_fds[slot][i].active) {
                newfd = i + PIPE_FD_BASE;
                pipe_fds[slot][i] = pipe_fds[slot][oldfd >= PIPE_FD_BASE ? oldfd - PIPE_FD_BASE : 0];
                break;
            }
        }
        result = newfd;
        break;
    }
    case SYS_DUP2: {
        int oldfd = (int)seL4_GetMR(1);
        int newfd = (int)seL4_GetMR(2);
        if (newfd >= PIPE_FD_BASE && newfd < PIPE_FD_BASE + MAX_PIPE_FDS) {
            pipe_fds[slot][newfd - PIPE_FD_BASE] = pipe_fds[slot][oldfd >= PIPE_FD_BASE ? oldfd - PIPE_FD_BASE : 0];
            result = newfd;
        } else {
            result = -1;
        }
        break;
    }
    case SYS_PIPE: {
        /* Allocate a pipe */
        int pi = -1;
        for (int i = 0; i < MAX_PIPES; i++) {
            if (!pipes[i].active) { pi = i; break; }
        }
        if (pi < 0) { result = -1; break; }
        pipes[pi].active = 1;
        pipes[pi].read_pos = 0;
        pipes[pi].write_pos = 0;
        pipes[pi].closed_write = 0;
        pipes[pi].closed_read = 0;
        /* Return read fd in MR1, write fd in MR2 */
        int rfd = -1, wfd = -1;
        for (int i = 0; i < MAX_PIPE_FDS; i++) {
            if (!pipe_fds[slot][i].active) {
                if (rfd < 0) {
                    rfd = i;
                    pipe_fds[slot][i].active = 1;
                    pipe_fds[slot][i].pipe_idx = pi;
                    pipe_fds[slot][i].is_write = 0;
                } else if (wfd < 0) {
                    wfd = i;
                    pipe_fds[slot][i].active = 1;
                    pipe_fds[slot][i].pipe_idx = pi;
                    pipe_fds[slot][i].is_write = 1;
                    break;
                }
            }
        }
        if (rfd < 0 || wfd < 0) {
            pipes[pi].active = 0;
            result = -1;
            break;
        }
        seL4_SetMR(1, (seL4_Word)(rfd + PIPE_FD_BASE));
        seL4_SetMR(2, (seL4_Word)(wfd + PIPE_FD_BASE));
        result = 0;
        break;
    }

    case SYS_SPAWN: {
        /* MR1 = flags (bit 0: background) */
        /* Path at sio+0x200, args at sio+0x600 */
        volatile char *path = (volatile char *)(sio + 0x200);
        volatile char *spawn_args = (volatile char *)(sio + 0x600);
        uint32_t flags = (uint32_t)seL4_GetMR(1);
        (void)flags;  /* TODO: use for background spawn */
        char fname[256], sargs[256];
        int i = 0;
        while (path[i] && i < 255) { fname[i] = path[i]; i++; }
        fname[i] = '\0';
        i = 0;
        while (spawn_args[i] && i < 255) { sargs[i] = spawn_args[i]; i++; }
        sargs[i] = '\0';

        uint32_t ppid = proctab[slot].pid;
        int child_slot = internal_spawn(fname, sargs, ppid, 0 /* always bg for spawned */);
        if (child_slot < 0) {
            result = -1;
        } else {
            result = (int64_t)proctab[child_slot].pid;
        }
        break;
    }
    case SYS_WAITPID: {
        /* MR1 = pid to wait for (0 = any child) */
        uint32_t wait_pid = (uint32_t)seL4_GetMR(1);
        uint32_t my_pid = proctab[slot].pid;

        /* Check exit_queue for already-exited child */
        int found = 0;
        for (int i = 0; i < MAX_EXIT_QUEUE; i++) {
            if (exit_queue[i].active && exit_queue[i].parent_pid == my_pid) {
                if (wait_pid == 0 || exit_queue[i].pid == wait_pid) {
                    result = (int64_t)exit_queue[i].pid;
                    seL4_SetMR(1, (seL4_Word)exit_queue[i].exit_code);
                    exit_queue[i].active = 0;
                    found = 1;
                    break;
                }
            }
        }
        if (!found) {
            /* Child hasn't exited yet — return EAGAIN so sandbox can poll */
            result = -2;  /* -2 = EAGAIN, try again later */
            seL4_SetMR(1, 0);
        }
        break;
    }
    case SYS_KILL_PROC: {
        /* MR1 = target PID, MR2 = signal number */
        uint32_t target_pid = (uint32_t)seL4_GetMR(1);
        uint32_t sig = (uint32_t)seL4_GetMR(2);
        (void)sig; /* For now, all signals terminate the process */
        int found = 0;
        for (int s = 0; s < NUM_SANDBOXES; s++) {
            if (proctab[s].in_use && proctab[s].pid == target_pid) {
                /* Permission check */
                if (proctab[s].owner_uid != current_uid && current_uid != 0) {
                    result = -1;  /* EPERM */
                    found = 1;
                    break;
                }
                microkit_pd_stop(s);
                microkit_pd_restart(s, SBX_ENTRY_POINT);
                /* Record exit for waitpid */
                for (int ei = 0; ei < MAX_EXIT_QUEUE; ei++) {
                    if (!exit_queue[ei].active) {
                        exit_queue[ei].active = 1;
                        exit_queue[ei].pid = proctab[s].pid;
                        exit_queue[ei].parent_pid = proctab[s].parent_pid;
                        exit_queue[ei].exit_code = -9;  /* killed */
                        break;
                    }
                }
                proctab[s].in_use = 0;
                result = 0;
                found = 1;
                break;
            }
        }
        if (!found) result = -1;  /* ESRCH */
        break;
    }

    case SYS_CHMOD: {
        volatile char *path = (volatile char *)(sio + 0x200);
        char raw[256];
        int i = 0;
        while (path[i] && i < 255) { raw[i] = path[i]; i++; }
        raw[i] = '\0';
        char fname[256];
        resolve_path(raw, fname, sizeof(fname));
        uint32_t mode = (uint32_t)seL4_GetMR(1);
        /* Permission check: owner or root */
        if (current_uid != 0) {
            if (auth_check_file_sync(fname, ACCESS_WRITE) != 0) {
                result = -13;
                break;
            }
        }
        volatile char *dst = (volatile char *)(fs_data + FS_FILENAME);
        i = 0;
        while (fname[i] && i < 63) { dst[i] = fname[i]; i++; }
        dst[i] = '\0';
        WR32(fs_data, FS_LENGTH, mode);
        WR32(fs_data, FS_CMD, FS_CMD_CHMOD);
        microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
        result = (int)RD32(fs_data, FS_STATUS);
        break;
    }
    case SYS_CHOWN: {
        volatile char *path = (volatile char *)(sio + 0x200);
        char raw[256];
        int i = 0;
        while (path[i] && i < 255) { raw[i] = path[i]; i++; }
        raw[i] = '\0';
        char fname[256];
        resolve_path(raw, fname, sizeof(fname));
        uint32_t uid = (uint32_t)seL4_GetMR(1);
        uint32_t gid = (uint32_t)seL4_GetMR(2);
        /* Only root can chown */
        if (current_uid != 0) {
            result = -1;
            break;
        }
        volatile char *dst = (volatile char *)(fs_data + FS_FILENAME);
        i = 0;
        while (fname[i] && i < 63) { dst[i] = fname[i]; i++; }
        dst[i] = '\0';
        WR32(fs_data, FS_UID, uid);
        WR32(fs_data, FS_GID, gid);
        WR32(fs_data, FS_CMD, FS_CMD_CHOWN);
        microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
        result = (int)RD32(fs_data, FS_STATUS);
        break;
    }
    case SYS_FTRUNCATE: {
        /* MR1 = fd, MR2 = length */
        uint32_t fd = (uint32_t)seL4_GetMR(1);
        uint32_t length = (uint32_t)seL4_GetMR(2);
        /* For now, truncate is a no-op that returns success */
        (void)fd; (void)length;
        result = 0;
        break;
    }
    case SYS_LINK: {
        /* Hard links not supported on this ext2 implementation */
        result = -1;
        break;
    }
    case SYS_FCNTL: {
        /* MR1 = fd, MR2 = cmd */
        uint32_t fd = (uint32_t)seL4_GetMR(1);
        uint32_t cmd = (uint32_t)seL4_GetMR(2);
        (void)fd;
        if (cmd == 1) { /* F_GETFD */
            result = 0;
        } else if (cmd == 2) { /* F_SETFD */
            result = 0;
        } else if (cmd == 3) { /* F_GETFL */
            result = 2; /* O_RDWR */
        } else if (cmd == 4) { /* F_SETFL */
            result = 0;
        } else {
            result = -1;
        }
        break;
    }
    case SYS_KILL: {
        /* kill(pid, sig) - same as KILL_PROC but standard POSIX name */
        uint32_t target_pid = (uint32_t)seL4_GetMR(1);
        uint32_t sig = (uint32_t)seL4_GetMR(2);
        int found = 0;
        for (int s = 0; s < NUM_SANDBOXES; s++) {
            if (proctab[s].in_use && proctab[s].pid == target_pid) {
                if (proctab[s].owner_uid != current_uid && current_uid != 0) {
                    result = -1;
                    found = 1;
                    break;
                }
                if (sig == 0) {
                    /* Signal 0: just check if process exists */
                    result = 0;
                    found = 1;
                } else {
                    microkit_pd_stop(s);
                    microkit_pd_restart(s, SBX_ENTRY_POINT);
                    for (int ei = 0; ei < MAX_EXIT_QUEUE; ei++) {
                        if (!exit_queue[ei].active) {
                            exit_queue[ei].active = 1;
                            exit_queue[ei].pid = proctab[s].pid;
                            exit_queue[ei].parent_pid = proctab[s].parent_pid;
                            exit_queue[ei].exit_code = -(int)sig;
                            break;
                        }
                    }
                    proctab[s].in_use = 0;
                    result = 0;
                    found = 1;
                }
                break;
            }
        }
        if (!found) result = -3; /* ESRCH */
        break;
    }
    case SYS_TIME: {
        /* Read ARM generic timer */
        uint64_t cnt, freq;
        asm volatile("mrs %0, cntpct_el0" : "=r"(cnt));
        asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
        if (freq == 0) freq = 62500000; /* QEMU virt default */
        uint64_t secs = cnt / freq;
        /* Epoch base: 2025-01-01 00:00:00 UTC */
        result = (int64_t)(1735689600ULL + secs);
        break;
    }

    case SYS_SIGNAL: {
        /* Stub: accept signal registration, do nothing */
        /* MR1 = signum, MR2 = handler address */
        result = 0; /* return previous handler (SIG_DFL = 0) */
        break;
    }

    case SYS_TRUNCATE: {
        volatile char *path = (volatile char *)(sio + 0x200);
        char raw[256];
        int i = 0;
        while (path[i] && i < 255) { raw[i] = path[i]; i++; }
        raw[i] = '\0';
        char fname[256];
        resolve_path(raw, fname, sizeof(fname));
        /* truncate = open + ftruncate + close */
        int st = fs_open_sync(fname);
        if (st == 0) {
            uint32_t fd = RD32(fs_data, FS_FD);
            /* length in MR1 */
            uint32_t len = (uint32_t)seL4_GetMR(1);
            (void)len; /* TODO: implement actual truncation in ext2 */
            fs_close_sync(fd);
            result = 0;
        } else {
            result = -1;
        }
        break;
    }

    case SYS_OPENDIR: {
        /* Same as READDIR — directory listing doesn't need a separate open */
        result = 0;
        break;
    }

    case SYS_CLOSEDIR: {
        /* No-op — directory state is client-side */
        result = 0;
        break;
    }

    case SYS_BRK: {
        /* Stub: sandbox has fixed heap, return success */
        result = 0;
        break;
    }

    default:
        result = -1;
        break;
    }

    seL4_SetMR(0, result);
    return microkit_msginfo_new(0, 6);
}

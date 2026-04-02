#include "aios/elf_loader.h"

/* Exec and spawn infrastructure
 * Auto-extracted from orchestrator.c
 * Lines 1097-1293
 */

/* ── Command: exec <file.bin> ────────────────────────── */

/* ── Wait/spawn infrastructure ────────────────────────── */
#define MAX_WAIT_QUEUE 8
typedef struct {
    int active;
    int waiting_slot;      /* slot of the parent that is blocked */
    uint32_t wait_pid;     /* PID the parent is waiting for (0 = any child) */
} wait_entry_t;
__attribute__((unused)) static wait_entry_t wait_queue[MAX_WAIT_QUEUE];

/* Completed child exit codes (ring buffer for waitpid) */
#define MAX_EXIT_QUEUE 16
typedef struct {
    int active;
    uint32_t pid;
    uint32_t parent_pid;
    int32_t exit_code;
} exit_record_t;
static exit_record_t exit_queue[MAX_EXIT_QUEUE];

/* Internal: spawn a program into a new slot, return slot or -1 */
static int internal_spawn(const char *filename, const char *args,
                          uint32_t parent_pid, int is_foreground) {
    int st = fs_open_sync(filename);
    /* Fallback: try /bin/<filename> if direct open fails */
    if (st != 0) {
        char bin_path[272];
        int j = 0;
        bin_path[j++] = '/'; bin_path[j++] = 'b'; bin_path[j++] = 'i'; bin_path[j++] = 'n'; bin_path[j++] = '/';
        for (int k = 0; filename[k] && j < 271; k++) bin_path[j++] = filename[k];
        bin_path[j] = '\0';
        st = fs_open_sync(bin_path);
    }
    if (st != 0) return -1;
    uint32_t fd = RD32(fs_data, FS_FD);
    uint32_t size = RD32(fs_data, FS_FILESIZE);
    if (size == 0 || size > 0x400000) {
        fs_close_sync(fd);
        return -1;
    }

    /* Read first bytes to detect file type */
    fs_read_sync(fd, 0, 4);
    {
        volatile uint8_t *hdr = (volatile uint8_t *)(fs_data + FS_DATA);
        if (hdr[0] == 0x23 && hdr[1] == 0x21) { /* #! shebang */
            fs_close_sync(fd);
            return -2; /* script file, not binary */
        }
    }

    int slot = find_free_slot();
    if (slot < 0) {
        /* All slots full — try to preempt a background victim */
        int victim_slot = sched_pick_victim(&sched);
        if (victim_slot < 0) {
            fs_close_sync(fd);
            return -1;  /* No preemptable process */
        }
        int victim_idx = sched.slot_to_proc[victim_slot];
        if (victim_idx < 0) {
            fs_close_sync(fd);
            return -1;
        }
        sched_proc_t *vp = &sched.procs[victim_idx];
        /* Save victim code+heap to swap */
        uint32_t code_sz = vp->loaded_bytes + 64*1024;
        if (code_sz > SCHED_CODE_SIZE) code_sz = SCHED_CODE_SIZE;
        uint32_t heap_sz = 256*1024;  /* save first 256K of heap */
        if (sched_swap_alloc(&sched, victim_idx, code_sz, heap_sz) < 0) {
            fs_close_sync(fd);
            return -1;  /* Swap full */
        }
        /* Set suspend flag — victim on another core may see it before we stop */
        WR32(sbx_io[victim_slot], SBX_SUSPEND_FLAG, 1);
        /* Brief yield to let victim on other core respond */
        for (volatile int d = 0; d < 1000; d++) {}
        microkit_pd_stop(victim_slot);
        /* Check if victim has valid saved state */
        {
            volatile uint32_t *ps_magic = (volatile uint32_t *)sbx_heap[victim_slot];
            if (*ps_magic == 0x50524F43U) {  /* PROC_STATE_MAGIC */
                /* Valid state — save to swap for later resume */
                volatile uint8_t *swap = (volatile uint8_t *)swap_region;
                volatile uint8_t *vc = (volatile uint8_t *)sbx_code[victim_slot];
                for (uint32_t j = 0; j < code_sz; j++) swap[vp->swap_offset + j] = vc[j];
                volatile uint32_t *ps_hu = (volatile uint32_t *)(sbx_heap[victim_slot] + 8);
                uint32_t real_heap = *ps_hu + 4096;
                if (real_heap > heap_sz) heap_sz = real_heap;
                if (heap_sz > SCHED_HEAP_SIZE) heap_sz = SCHED_HEAP_SIZE;
                volatile uint8_t *vh = (volatile uint8_t *)sbx_heap[victim_slot];
                uint32_t ho = vp->swap_offset + code_sz;
                for (uint32_t j = 0; j < heap_sz; j++) swap[ho + j] = vh[j];
                vp->swap_code_sz = code_sz;
                vp->swap_heap_sz = heap_sz;
                vp->state = PROC_READY;
            } else {
                /* No valid state — requeue for restart from disk */
                int vi = sched.slot_to_proc[victim_slot];
                if (vi >= 0) sched_swap_free(&sched, vi);
                vp->state = PROC_QUEUED;
            }
        }
        vp->slot = -1;
        sched.slot_to_proc[victim_slot] = -1;
        microkit_pd_stop(victim_slot);  /* stop old PD; restart after code loaded */
        /* Clear stale proc_state magic so fresh program sees clean heap */
        WR32(sbx_heap[slot], 0, 0);  /* zero magic field */
        slot = victim_slot;
    }

    /* Clear stale state before loading new program */
    WR32(sbx_heap[slot], 0, 0);  /* clear proc_state magic */
    WR32(sbx_io[slot], SBX_SUSPEND_FLAG, 0);  /* clear suspend flag */
    uintptr_t cur_sio = sbx_io[slot];
    uintptr_t cur_scode = sbx_code[slot];
    volatile uint8_t *code_dst = (volatile uint8_t *)cur_scode;

    /* Read entire file into code region (temporary staging area) */
    uint32_t offset = 0;
    while (offset < size) {
        uint32_t chunk = size - offset;
        if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
        st = fs_read_sync(fd, offset, chunk);
        if (st != 0 && st != 3) break;
        uint32_t got = RD32(fs_data, FS_LENGTH);
        if (got == 0) break;
        volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
        for (uint32_t j = 0; j < got; j++) code_dst[offset + j] = src[j];
        offset += got;
    }
    fs_close_sync(fd);

    /* Detect and handle ELF files */
    if (offset >= 64 && elf_is_valid((const uint8_t *)code_dst, offset)) {
        /* ELF binary — parse and load segments */
        /* Make a copy of file data since we load in-place */
        /* For small files (< 2MB), use upper half of code region as temp */
        uint32_t code_max = 0x400000;  /* 4 MiB */
        uint32_t temp_off = code_max / 2;  /* Use upper 2MB as temp */
        if (offset > temp_off) {
            /* File too large for in-place reload — load directly */
            /* Already in code_dst, treat as raw */
            goto raw_loaded;
        }
        /* Copy raw file to temp area */
        for (uint32_t j = 0; j < offset; j++)
            code_dst[temp_off + j] = code_dst[j];
        /* Clear the load area */
        for (uint32_t j = 0; j < temp_off; j++)
            code_dst[j] = 0;

        elf_load_result_t elf_result;
        int elf_rc = elf_load((const uint8_t *)(code_dst + temp_off), offset,
                              code_dst, 0x21100000, code_max / 2, &elf_result);
        if (elf_rc == 0) {
            offset = elf_result.total_memsz;
            if (elf_result.is_linux) {
                ser_puts("  ELF: Linux binary rebased to 0x21100000");
            } else {
                ser_puts("  ELF: native AIOS binary");
            }
            ser_puts(" (");
            ser_put_dec(elf_result.num_segments);
            ser_puts(" segments, ");
            ser_put_dec(elf_result.total_memsz);
            ser_puts(" bytes)\n");
            ser_flush();
        } else {
            ser_puts("  ELF: load failed (rc=");
            ser_put_dec((unsigned int)(-elf_rc));
            ser_puts("), falling back to raw\n");
            ser_flush();
            /* Restore raw data from temp */
            for (uint32_t j = 0; j < offset; j++)
                code_dst[j] = code_dst[temp_off + j];
        }
    }
raw_loaded:
    (void)0;

    /* Copy args */
    {
        volatile char *adst = (volatile char *)(cur_sio + SBX_ARGS);
        int ai = 0;
        if (args) {
            while (args[ai] && ai < SBX_ARGS_MAX - 1) {
                adst[ai] = args[ai]; ai++;
            }
        }
        adst[ai] = '\0';
    }

    /* Start the sandbox */
    WR32(cur_sio, SBX_CODE_SIZE, offset);
    WR32(cur_sio, SBX_CMD, SBX_CMD_RUN);
    /* Create process entry and assign to slot */
    {
        int pid = sched_create(&sched, filename, "", parent_pid,
                               current_uid, is_foreground, 128);
        if (pid < 0) { return -1; }
        /* Find the proc index we just created */
        for (int pi = 0; pi < SCHED_MAX_PROCS; pi++) {
            if (sched.procs[pi].pid == pid) {
                sched.procs[pi].loaded_bytes = offset;
                sched_assign_slot(&sched, pi, slot);
                break;
            }
        }
    }
    microkit_notify(sbx_channel[slot]);
    return slot;
}

static void cmd_exec(const char *filename) {
    /* Check for background execution */
    int background = 0;
    char fname_buf[256];
    {
        int len = 0;
        while (filename[len] && len < 255) { fname_buf[len] = filename[len]; len++; }
        fname_buf[len] = 0;
        /* Strip trailing spaces and & */
        while (len > 0 && fname_buf[len-1] == ' ') fname_buf[--len] = 0;
        if (len > 0 && fname_buf[len-1] == '&') {
            fname_buf[--len] = 0;
            while (len > 0 && fname_buf[len-1] == ' ') fname_buf[--len] = 0;
            background = 1;
        }
        filename = fname_buf;
    }

    int st = fs_open_sync(filename);
    /* Fallback: try /bin/<filename> if direct open fails */
    if (st != 0) {
        char bp[272];
        int j = 0;
        bp[j++] = '/'; bp[j++] = 'b'; bp[j++] = 'i'; bp[j++] = 'n'; bp[j++] = '/';
        for (int k = 0; filename[k] && j < 271; k++) bp[j++] = filename[k];
        bp[j] = '\0';
        st = fs_open_sync(bp);
    }
    if (st != 0) {
        ser_puts("  File not found.\n"); ser_flush();
        return;
    }
    uint32_t fd   = RD32(fs_data, FS_FD);
    uint32_t size = RD32(fs_data, FS_FILESIZE);

    if (size > 0x400000) {
        ser_puts("  Error: program too large (max 1 MiB)\n");
        ser_flush();
        fs_close_sync(fd);
        return;
    }

    /* Check for shebang — reject scripts */
    fs_read_sync(fd, 0, 4);
    {
        volatile uint8_t *hdr = (volatile uint8_t *)(fs_data + FS_DATA);
        if (hdr[0] == 0x23 && hdr[1] == 0x21) { /* #! */
            ser_puts("  Error: script file, not a binary\n");
            ser_flush();
            fs_close_sync(fd);
            return;
        }
    }

    /* Load into sandbox_code */
    int slot = find_free_slot();
    if (slot < 0) {
        /* Try preemption */
        int victim_slot = sched_pick_victim(&sched);
        if (victim_slot < 0) {
            ser_puts("  Error: no free process slots\n");
            ser_flush();
            fs_close_sync(fd);
            return;
        }
        int victim_idx = sched.slot_to_proc[victim_slot];
        if (victim_idx < 0) {
            ser_puts("  Error: no free process slots\n");
            ser_flush();
            fs_close_sync(fd);
            return;
        }
        sched_proc_t *vp = &sched.procs[victim_idx];
        uint32_t code_sz = vp->loaded_bytes + 64*1024;
        if (code_sz > SCHED_CODE_SIZE) code_sz = SCHED_CODE_SIZE;
        uint32_t heap_sz = 256*1024;
        if (sched_swap_alloc(&sched, victim_idx, code_sz, heap_sz) < 0) {
            ser_puts("  Error: swap region full\n");
            ser_flush();
            fs_close_sync(fd);
            return;
        }
        /* Set suspend flag — victim on another core may see it before we stop */
        WR32(sbx_io[victim_slot], SBX_SUSPEND_FLAG, 1);
        /* Brief yield to let victim on other core respond */
        for (volatile int d = 0; d < 1000; d++) {}
        microkit_pd_stop(victim_slot);
        /* Check if victim has valid saved state */
        {
            volatile uint32_t *ps_magic = (volatile uint32_t *)sbx_heap[victim_slot];
            if (*ps_magic == 0x50524F43U) {  /* PROC_STATE_MAGIC */
                /* Valid state — save to swap for later resume */
                volatile uint8_t *swap = (volatile uint8_t *)swap_region;
                volatile uint8_t *vc = (volatile uint8_t *)sbx_code[victim_slot];
                for (uint32_t j = 0; j < code_sz; j++) swap[vp->swap_offset + j] = vc[j];
                volatile uint32_t *ps_hu = (volatile uint32_t *)(sbx_heap[victim_slot] + 8);
                uint32_t real_heap = *ps_hu + 4096;
                if (real_heap > heap_sz) heap_sz = real_heap;
                if (heap_sz > SCHED_HEAP_SIZE) heap_sz = SCHED_HEAP_SIZE;
                volatile uint8_t *vh = (volatile uint8_t *)sbx_heap[victim_slot];
                uint32_t ho = vp->swap_offset + code_sz;
                for (uint32_t j = 0; j < heap_sz; j++) swap[ho + j] = vh[j];
                vp->swap_code_sz = code_sz;
                vp->swap_heap_sz = heap_sz;
                vp->state = PROC_READY;
            } else {
                /* No valid state — requeue for restart from disk */
                int vi = sched.slot_to_proc[victim_slot];
                if (vi >= 0) sched_swap_free(&sched, vi);
                vp->state = PROC_QUEUED;
            }
        }
        vp->slot = -1;
        sched.slot_to_proc[victim_slot] = -1;
        microkit_pd_stop(victim_slot);  /* stop old PD; restart after code loaded */
        /* Clear stale proc_state magic so fresh program sees clean heap */
        WR32(sbx_heap[slot], 0, 0);  /* zero magic field */
        slot = victim_slot;
        ser_puts("  Preempted PID ");
        ser_put_dec((unsigned int)vp->pid);
        ser_puts(" from slot ");
        ser_put_dec((unsigned int)victim_slot);
        ser_puts("\n");
    }
    /* Clear stale state before loading new program */
    WR32(sbx_heap[slot], 0, 0);  /* clear proc_state magic */
    WR32(sbx_io[slot], SBX_SUSPEND_FLAG, 0);  /* clear suspend flag */
    uintptr_t cur_sio = sbx_io[slot];
    uintptr_t cur_scode = sbx_code[slot];
    volatile uint8_t *code_dst = (volatile uint8_t *)cur_scode;
    uint32_t offset = 0;
    while (offset < size) {
        uint32_t chunk = size - offset;
        if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
        st = fs_read_sync(fd, offset, chunk);
        if (st != 0 && st != 3) break;
        uint32_t got = RD32(fs_data, FS_LENGTH);
        if (got == 0) break;
        volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
        for (uint32_t i = 0; i < got; i++) code_dst[offset + i] = src[i];
        offset += got;
    }
    fs_close_sync(fd);

    /* Detect and handle ELF files */
    if (offset >= 64 && elf_is_valid((const uint8_t *)code_dst, offset)) {
        uint32_t code_max = 0x400000;
        uint32_t temp_off = code_max / 2;
        if (offset <= temp_off) {
            for (uint32_t j = 0; j < offset; j++)
                code_dst[temp_off + j] = code_dst[j];
            for (uint32_t j = 0; j < temp_off; j++)
                code_dst[j] = 0;

            elf_load_result_t elf_result;
            int elf_rc = elf_load((const uint8_t *)(code_dst + temp_off), offset,
                                  code_dst, 0x21100000, code_max / 2, &elf_result);
            if (elf_rc == 0) {
                offset = elf_result.total_memsz;
                if (elf_result.is_linux) {
                    ser_puts("  ELF: Linux binary rebased to 0x21100000");
                } else {
                    ser_puts("  ELF: native AIOS binary");
                }
                ser_puts(" (");
                ser_put_dec(elf_result.num_segments);
                ser_puts(" segments, ");
                ser_put_dec(elf_result.total_memsz);
                ser_puts(" bytes)\n");
                ser_flush();
            } else {
                ser_puts("  ELF: load failed, falling back to raw\n");
                ser_flush();
                for (uint32_t j = 0; j < offset; j++)
                    code_dst[j] = code_dst[temp_off + j];
            }
        }
    }

    exec_loaded_bytes = offset;


    /* Copy args to sandbox_io for the program */
    {
        volatile char *adst = (volatile char *)(cur_sio + SBX_ARGS);
        int ai = 0;
        while (exec_args[ai] && ai < SBX_ARGS_MAX - 1) {
            adst[ai] = exec_args[ai]; ai++;
        }
        adst[ai] = '\0';
    }

    /* Tell sandbox to run */
    WR32(cur_sio, SBX_CODE_SIZE, exec_loaded_bytes);
    WR32(cur_sio, SBX_CMD, SBX_CMD_RUN);
    /* Create process entry and assign to slot */
    {
        int pid = sched_create(&sched, filename, "", 0,
                               current_uid, background ? 0 : 1, 128);
        if (pid >= 0) {
            for (int pi = 0; pi < SCHED_MAX_PROCS; pi++) {
                if (sched.procs[pi].pid == pid) {
                    sched.procs[pi].loaded_bytes = exec_loaded_bytes;
                    sched_assign_slot(&sched, pi, slot);
                    break;
                }
            }
        }
    }
    /* Name set by sched_create */
    if (!background) {
        orch_state = EXEC_RUNNING;
    } else {
        ser_puts("  [");
        ser_put_dec((unsigned int)slot);
        ser_puts("] PID ");
        ser_put_dec(slot_proc(slot) ? slot_proc(slot)->pid : 0);
        ser_puts("\n");
        print_prompt();
    }
    microkit_notify(sbx_channel[slot]);
}

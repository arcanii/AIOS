/*
 * display_server.c -- Framebuffer display server
 *
 * IPC server for user programs to draw on the ramfb framebuffer.
 * Runs in the root task with direct access to gpu_fb[].
 */
#include "aios/root_shared.h"
#include "aios/gpu.h"
#include "aios/vfs.h"
#define LOG_MODULE "disp"
#define LOG_LEVEL LOG_LEVEL_DEBUG
#include "aios/aios_log.h"
#include <stdio.h>

/* Font and drawing functions from boot_display_init.c */
extern const uint8_t font8x8[95][8];
extern void gpu_draw_char(int x, int y, char ch,
                          uint32_t fg, int scale);
extern void gpu_draw_text(int x, int y, const char *str,
                          uint32_t fg, int scale);

/* Extract a string from IPC message registers starting at mr_start.
 * len = byte count, result written to buf (null-terminated). */
static void ipc_extract_str(char *buf, int len, int mr_start) {
    if (len > 255) len = 255;
    for (int i = 0; i < len; i++) {
        seL4_Word w = seL4_GetMR(mr_start + i / 8);
        buf[i] = (char)((w >> ((i % 8) * 8)) & 0xFF);
    }
    buf[len] = '\0';
}

void display_server_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg0;
    (void)arg1; (void)ipc_buf;

    printf("[disp] Display server ready (%ux%u)\n", gpu_width, gpu_height);

    while (1) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);

        switch (label) {

        case DISP_FB_INFO: {
            seL4_SetMR(0, gpu_width);
            seL4_SetMR(1, gpu_height);
            seL4_SetMR(2, gpu_available);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 3));
            break;
        }

        case DISP_SHOW_FILE: {
            int pathlen = (int)seL4_GetMR(0);
            char path[256];
            ipc_extract_str(path, pathlen, 1);

            if (!gpu_available || !gpu_fb) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            int img_size = vfs_read(path, elf_buf, 8 * 1024 * 1024);
            if (img_size < 16) {
                printf("[disp] %s: not found or too small (%d)\n", path, img_size);
                seL4_SetMR(0, (seL4_Word)-2);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            uint32_t *hdr = (uint32_t *)elf_buf;
            uint32_t iw = hdr[0], ih = hdr[1], fmt = hdr[2];
            if (fmt != 0 || iw == 0 || ih == 0 || iw > 4096 || ih > 4096) {
                printf("[disp] %s: bad format/dims\n", path);
                seL4_SetMR(0, (seL4_Word)-3);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            uint32_t *pixels = (uint32_t *)(elf_buf + 16);
            uint32_t ox = (iw < gpu_width)  ? (gpu_width  - iw) / 2 : 0;
            uint32_t oy = (ih < gpu_height) ? (gpu_height - ih) / 2 : 0;
            uint32_t cw = (iw < gpu_width)  ? iw : gpu_width;
            uint32_t ch = (ih < gpu_height) ? ih : gpu_height;

            for (uint32_t y = 0; y < ch; y++)
                for (uint32_t x = 0; x < cw; x++)
                    gpu_fb[(oy + y) * gpu_width + (ox + x)] =
                        pixels[y * iw + x];

            printf("[disp] Displayed %s (%ux%u)\n", path, iw, ih);
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }

        case DISP_TEXT: {
            if (!gpu_available || !gpu_fb) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int x     = (int)seL4_GetMR(0);
            int y     = (int)seL4_GetMR(1);
            int scale = (int)seL4_GetMR(2);
            uint32_t color = (uint32_t)seL4_GetMR(3);
            int tlen  = (int)seL4_GetMR(4);
            char text[128];
            ipc_extract_str(text, tlen > 127 ? 127 : tlen, 5);
            if (scale < 1) scale = 1;
            if (scale > 8) scale = 8;
            gpu_draw_text(x, y, text, color, scale);
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }

        case DISP_FILL_RECT: {
            if (!gpu_available || !gpu_fb) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            uint32_t rx = (uint32_t)seL4_GetMR(0);
            uint32_t ry = (uint32_t)seL4_GetMR(1);
            uint32_t rw = (uint32_t)seL4_GetMR(2);
            uint32_t rh = (uint32_t)seL4_GetMR(3);
            uint32_t rc = (uint32_t)seL4_GetMR(4);
            if (rx + rw > gpu_width)  rw = gpu_width  - rx;
            if (ry + rh > gpu_height) rh = gpu_height - ry;
            for (uint32_t y = ry; y < ry + rh; y++)
                for (uint32_t x = rx; x < rx + rw; x++)
                    gpu_fb[y * gpu_width + x] = rc;
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }

        case DISP_CLEAR: {
            if (!gpu_available || !gpu_fb) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            uint32_t cc = (uint32_t)seL4_GetMR(0);
            uint32_t total = gpu_width * gpu_height;
            for (uint32_t i = 0; i < total; i++)
                gpu_fb[i] = cc;
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }

        default:
            seL4_SetMR(0, (seL4_Word)-99);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
    }
}

/*
 * fbshow.c -- Display raw images on the framebuffer
 *
 * Usage: fbshow <file.raw>     Show raw image (centered)
 *        fbshow --info         Print framebuffer info
 *        fbshow --clear        Clear screen to black
 *        fbshow --clear RRGGBB Clear screen to hex color
 */
#include <stdio.h>
#include <sel4/sel4.h>
#include "posix_internal.h"
#include "aios/root_shared.h"

static void pack_path(const char *path, int len) {
    seL4_SetMR(0, (seL4_Word)len);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < len; i++) {
        w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == len - 1) {
            seL4_SetMR(mr++, w);
            w = 0;
        }
    }
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static uint32_t hex_to_u32(const char *s) {
    uint32_t v = 0;
    for (int i = 0; i < 6 && s[i]; i++) {
        char c = s[i];
        int d = 0;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        v = (v << 4) | d;
    }
    return v;
}

int main(int argc, char *argv[]) {
    if (!disp_ep) {
        printf("fbshow: no display endpoint\n");
        return 1;
    }

    if (argc < 2) {
        printf("Usage: fbshow <file.raw> | --info | --clear [RRGGBB]\n");
        return 1;
    }

    if (streq(argv[1], "--info")) {
        seL4_MessageInfo_t reply = seL4_Call(disp_ep,
            seL4_MessageInfo_new(DISP_FB_INFO, 0, 0, 0));
        (void)reply;
        uint32_t w = (uint32_t)seL4_GetMR(0);
        uint32_t h = (uint32_t)seL4_GetMR(1);
        uint32_t a = (uint32_t)seL4_GetMR(2);
        printf("%ux%u XRGB8888 (active=%u)\n", w, h, a);
        return 0;
    }

    if (streq(argv[1], "--clear")) {
        uint32_t color = 0;
        if (argc > 2) color = hex_to_u32(argv[2]);
        seL4_SetMR(0, color);
        seL4_Call(disp_ep, seL4_MessageInfo_new(DISP_CLEAR, 0, 0, 1));
        return 0;
    }

    /* Show raw image file */
    const char *path = argv[1];
    int len = 0;
    while (path[len]) len++;

    pack_path(path, len);
    int mr_count = 1 + (len + 7) / 8;
    seL4_MessageInfo_t reply = seL4_Call(disp_ep,
        seL4_MessageInfo_new(DISP_SHOW_FILE, 0, 0, mr_count));
    (void)reply;
    int result = (int)seL4_GetMR(0);
    if (result != 0)
        printf("fbshow: display failed (%d)\n", result);
    return result;
}

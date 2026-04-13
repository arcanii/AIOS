/*
 * fb_console.c -- Framebuffer text console
 *
 * Scrolling character-grid console rendered directly to gpu_fb.
 * Provides boot-time text output on HDMI when serial is unavailable
 * (e.g. RPi4 without USB-to-serial adapter).
 *
 * Grid: 128 cols x 76 rows (8px wide, 10px tall cells at 1024x768)
 * Font: 8x8 bitmap from boot_display_init.c (ASCII 32-126)
 *
 * All functions are no-ops if display was not initialized.
 */
#include "aios/fb_console.h"
#include "aios/root_shared.h"
#include "aios/gpu.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define FB_CELL_W   8       /* pixels per character width */
#define FB_CELL_H   10      /* pixels per character height (8 glyph + 2 spacing) */
#define FB_MAX_COLS 128     /* 1024 / 8 */
#define FB_MAX_ROWS 76      /* 768 / 10 */

/* Console state */
static int con_col;
static int con_row;
static int con_cols;        /* actual columns for current resolution */
static int con_rows;        /* actual rows for current resolution */
static int fb_con_active;

/* Colors */
static const uint32_t CON_FG = 0x00CCCCCC;  /* light gray */
static const uint32_t CON_BG = 0x00101820;  /* dark blue-black */

/* Font from boot_display_init.c -- 95 glyphs, ASCII 32-126 */
extern const uint8_t font8x8[95][8];

/* ---- Render one character cell to framebuffer ---- */
static void render_cell(int col, int row, char c, uint32_t fg) {
    if (!gpu_fb) return;

    int glyph_idx = (c >= 32 && c <= 126) ? c - 32 : 0;
    const uint8_t *g = font8x8[glyph_idx];
    int px = col * FB_CELL_W;
    int py = row * FB_CELL_H;
    uint32_t w = gpu_width;

    /* Draw 8x8 glyph */
    for (int r = 0; r < 8; r++) {
        uint8_t bits = g[r];
        uint32_t *row_ptr = &gpu_fb[(py + r) * w + px];
        for (int b = 0; b < 8; b++) {
            row_ptr[b] = (bits & (0x80 >> b)) ? fg : CON_BG;
        }
    }

    /* Clear 2px spacing below glyph */
    for (int r = 8; r < FB_CELL_H; r++) {
        uint32_t *row_ptr = &gpu_fb[(py + r) * w + px];
        for (int b = 0; b < FB_CELL_W; b++) {
            row_ptr[b] = CON_BG;
        }
    }
}

/* ---- Scroll up by one row ---- */
static void scroll_up(void) {
    if (!gpu_fb) return;

    uint32_t w = gpu_width;
    uint32_t shift_rows = (uint32_t)((con_rows - 1) * FB_CELL_H);
    uint32_t shift_pixels = shift_rows * w;
    uint32_t row_pixels = (uint32_t)(FB_CELL_H * w);

    /* Shift framebuffer pixels up by one cell row */
    memmove(gpu_fb, gpu_fb + row_pixels, shift_pixels * 4);

    /* Clear bottom row */
    uint32_t *bottom = gpu_fb + shift_pixels;
    uint32_t clear_count = row_pixels;
    for (uint32_t i = 0; i < clear_count; i++)
        bottom[i] = CON_BG;
}

/* ---- Advance to next line, scroll if needed ---- */
static void newline(void) {
    con_col = 0;
    con_row++;
    if (con_row >= con_rows) {
        scroll_up();
        con_row = con_rows - 1;
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

void fb_console_init(void) {
    if (!gpu_available || !gpu_fb) return;

    con_cols = (int)(gpu_width / FB_CELL_W);
    con_rows = (int)(gpu_height / FB_CELL_H);
    if (con_cols > FB_MAX_COLS) con_cols = FB_MAX_COLS;
    if (con_rows > FB_MAX_ROWS) con_rows = FB_MAX_ROWS;

    con_col = 0;
    con_row = 0;

    /* Fill framebuffer with background color */
    uint32_t total = gpu_width * gpu_height;
    for (uint32_t i = 0; i < total; i++)
        gpu_fb[i] = CON_BG;

    fb_con_active = 1;
}

void fb_console_putc(char c) {
    if (!fb_con_active) return;

    switch (c) {
    case '\n':
        newline();
        break;
    case '\r':
        con_col = 0;
        break;
    case '\b':
        if (con_col > 0) {
            con_col--;
            render_cell(con_col, con_row, ' ', CON_FG);
        }
        break;
    case '\t':
        /* Advance to next 8-column boundary */
        con_col = (con_col + 8) & ~7;
        if (con_col >= con_cols) newline();
        break;
    default:
        if (c >= 32 && c <= 126) {
            render_cell(con_col, con_row, c, CON_FG);
            con_col++;
            if (con_col >= con_cols) newline();
        }
        break;
    }
}

void fb_console_puts(const char *s) {
    if (!fb_con_active) return;
    while (*s)
        fb_console_putc(*s++);
}

void fb_console_printf(const char *fmt, ...) {
    if (!fb_con_active) return;

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    fb_console_puts(buf);
}

void fb_console_clear(void) {
    if (!fb_con_active) return;

    uint32_t total = gpu_width * gpu_height;
    for (uint32_t i = 0; i < total; i++)
        gpu_fb[i] = CON_BG;

    con_col = 0;
    con_row = 0;
}

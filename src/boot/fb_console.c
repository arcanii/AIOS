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

/* Text rows reserved for splash/boot header */
#define CON_HEADER_ROWS  8
#define FB_MAX_COLS 128     /* 1024 / 8 */
#define FB_MAX_ROWS 76      /* 768 / 10 */

/* Console state */
static int con_col;
static int con_row;
static int con_cols;        /* actual columns for current resolution */
static int con_rows;        /* actual rows for current resolution */
static int fb_con_active;
static int fb_con_suspended; /* 1 while the GPU owns the FB (V3D job in flight) */

/* Dirty pixel-row range [y0, y1) accumulated since the last flush. The framebuffer
 * is cacheable, so writes must be cleaned to PoC for the display to see them --
 * fb_console_flush() cleans exactly the dirty rows (called once per output batch). */
static int con_dirty;
static uint32_t con_dirty_y0, con_dirty_y1;
static void con_mark(uint32_t y0, uint32_t y1) {
    if (y1 > gpu_height) y1 = gpu_height;
    if (y0 >= y1) return;
    if (!con_dirty) { con_dirty_y0 = y0; con_dirty_y1 = y1; con_dirty = 1; }
    else {
        if (y0 < con_dirty_y0) con_dirty_y0 = y0;
        if (y1 > con_dirty_y1) con_dirty_y1 = y1;
    }
}

/* Scroll/flush diagnostics, read via /proc/fbcon. A real-HW freeze on the first scroll
 * (cacheable framebuffer) pins to one of these phases: read /proc/fbcon over netconsole
 * after a freeze (the net threads stay alive) to see exactly where display_server hung --
 * the scroll memmove, the per-page cache clean (and which page), or neither (IPC). */
volatile uint32_t fbcon_scroll_seq;    /* scrolls started */
volatile int      fbcon_phase;         /* 0 idle, 1 memmove, 2 clear, 3 mark, 4 flush, 5 done */
extern volatile uint32_t fbflush_seq, fbflush_pages_done, fbflush_pages_total;

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
    con_mark((uint32_t)py, (uint32_t)py + FB_CELL_H);
}

/* ---- Scroll up by one row ---- */
static void scroll_up(void) {
    if (!gpu_fb) return;
    fbcon_scroll_seq++;

    uint32_t w = gpu_width;
    uint32_t shift_rows = (uint32_t)((con_rows - 1) * FB_CELL_H);
    uint32_t shift_pixels = shift_rows * w;
    uint32_t row_pixels = (uint32_t)(FB_CELL_H * w);

    /* Shift framebuffer pixels up by one cell row */
    fbcon_phase = 1;   /* memmove */
    memmove(gpu_fb, gpu_fb + row_pixels, shift_pixels * 4);

    /* Clear bottom row */
    fbcon_phase = 2;   /* clear bottom */
    uint32_t *bottom = gpu_fb + shift_pixels;
    uint32_t clear_count = row_pixels;
    for (uint32_t i = 0; i < clear_count; i++)
        bottom[i] = CON_BG;

    fbcon_phase = 3;   /* mark */
    con_mark(0, (uint32_t)(con_rows * FB_CELL_H));   /* only the console area moved */
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
    con_row = CON_HEADER_ROWS;

    /* Clear framebuffer below the header area (preserve splash top) */
    uint32_t start_y = (uint32_t)CON_HEADER_ROWS * FB_CELL_H;
    for (uint32_t y = start_y; y < gpu_height; y++)
        for (uint32_t x = 0; x < gpu_width; x++)
            gpu_fb[y * gpu_width + x] = CON_BG;
    gpu_fb_flush_all();

    fb_con_active = 1;
}

/* Clean the dirty rows to the framebuffer. Called once per output batch (by
 * fb_console_puts and by the display server after a DISP_CONSOLE batch) so the
 * cached cell writes become visible without a clean-per-character cost. */
void fb_console_flush(void) {
    if (fb_con_suspended) { con_dirty = 0; return; }
    if (!con_dirty || !gpu_fb) { con_dirty = 0; return; }
    uint32_t w = gpu_width;
    fbcon_phase = 4;   /* flush (per-page clean; fbflush_pages_done shows progress) */
    gpu_fb_flush((uint8_t *)gpu_fb + (uint64_t)con_dirty_y0 * w * 4,
                 (con_dirty_y1 - con_dirty_y0) * w * 4);
    con_dirty = 0;
    fbcon_phase = 5;   /* done */
}

/* /proc/fbcon -- scroll/flush diagnostics for the HW freeze (see the counters above). */
int fb_console_diag(char *buf, int bufsize) {
    const char *ph[] = {"idle", "memmove", "clear", "mark", "flush", "done"};
    int p = (fbcon_phase >= 0 && fbcon_phase <= 5) ? fbcon_phase : 0;
    return snprintf(buf, bufsize,
        "active=%d %dx%d row=%d/%d col=%d\n"
        "scrolls=%u phase=%d(%s)\n"
        "flush: seq=%u pages=%u/%u\n",
        fb_con_active, con_cols, con_rows, con_row, con_rows, con_col,
        fbcon_scroll_seq, fbcon_phase, ph[p],
        fbflush_seq, fbflush_pages_done, fbflush_pages_total);
}

/* Suspend/resume console rendering. While suspended the GPU owns the framebuffer
 * (a V3D job is in flight), so every FB-touching console op no-ops -- a dirty CPU
 * line write must never land over GPU pixels (the FB cache-ownership protocol,
 * DESIGN sec 5). Serial still carries every byte: tty_server writes serial
 * independently of the HDMI mirror. */
void fb_console_set_suspend(int on) {
    fb_con_suspended = on ? 1 : 0;
    if (fb_con_suspended) con_dirty = 0;   /* drop any pending dirty range */
}

void fb_console_putc(char c) {
    if (!fb_con_active || fb_con_suspended) return;

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
    fb_console_flush();
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
    if (!fb_con_active || fb_con_suspended) return;

    uint32_t total = gpu_width * gpu_height;
    for (uint32_t i = 0; i < total; i++)
        gpu_fb[i] = CON_BG;
    gpu_fb_flush_all();

    con_col = 0;
    con_row = 0;
}

#ifndef AIOS_FB_CONSOLE_H
#define AIOS_FB_CONSOLE_H
/*
 * fb_console.h -- Framebuffer text console
 *
 * Scrolling character-grid console rendered to gpu_fb.
 * Used for boot status on HDMI when serial is unavailable.
 * All functions no-op if display not initialized.
 */

void fb_console_init(void);
void fb_console_putc(char c);
void fb_console_puts(const char *s);
void fb_console_printf(const char *fmt, ...);
void fb_console_clear(void);
/* Clean accumulated cell writes to the (cacheable) framebuffer. fb_console_puts/
 * printf call it automatically; batch callers (display_server DISP_CONSOLE) render
 * many fb_console_putc then call this once. */
void fb_console_flush(void);

/* Suspend/resume console rendering while the GPU owns the framebuffer (V3D job in
 * flight). When suspended every FB-touching console op no-ops so a dirty CPU line
 * never overwrites GPU pixels (the FB cache-ownership protocol). */
void fb_console_set_suspend(int on);

/* /proc/fbcon -- scroll/flush diagnostics (phase + flush page progress) for debugging the
 * cacheable-framebuffer scroll freeze on real HW. Writes a snapshot into buf. */
int fb_console_diag(char *buf, int bufsize);

#endif /* AIOS_FB_CONSOLE_H */

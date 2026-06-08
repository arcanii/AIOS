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

#endif /* AIOS_FB_CONSOLE_H */

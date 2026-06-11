/* HDMI debug for elfloader -- hex value display + bar fallback
 * Framebuffer info struct at physical 0x1000, written by diagnostic stub.
 *
 * diag_val(stage, v1, v2) -- one line: SS AAAAAAAA SSSSSSSS
 *   stage = green 2-digit hex, v1 = white 8-digit hex, v2 = cyan 8-digit hex
 * diag_bar(width, color) -- colored bar (legacy, still used in sys_boot.c)
 */

/* 3x5 pixel font for hex digits 0-F */
static const unsigned int _diag_hf[16][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},
    {5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},
    {7,5,7,5,7},{7,5,7,1,7},{7,5,7,5,5},{6,5,7,5,6},
    {7,4,4,4,7},{6,5,5,5,6},{7,4,7,4,7},{7,4,7,4,4},
};

static inline void _diag_digit(volatile unsigned int *fb, unsigned int stride,
                                unsigned int x, unsigned int y, unsigned int d,
                                unsigned int color, unsigned int sc) {
    if (d > 15) d = 0;
    for (unsigned int r = 0; r < 5; r++)
        for (unsigned int c = 0; c < 3; c++)
            if (_diag_hf[d][r] & (4 >> c))
                for (unsigned int sy = 0; sy < sc; sy++)
                    for (unsigned int sx = 0; sx < sc; sx++)
                        fb[(y + r*sc + sy) * stride + (x + c*sc + sx)] = color;
}

static inline void _diag_hex(volatile unsigned int *fb, unsigned int stride,
                              unsigned int x, unsigned int y, unsigned int val,
                              int ndigits, unsigned int color, unsigned int sc) {
    for (int i = ndigits - 1; i >= 0; i--)
        _diag_digit(fb, stride, x + (unsigned int)(ndigits-1-i) * (4*sc),
                    y, (val >> (i*4)) & 0xF, color, sc);
}

static inline void diag_val(unsigned int stage, unsigned int v1, unsigned int v2) {
    volatile struct { unsigned long ptr; unsigned int pitch, width, height, line; }
        *info = (void *)0x3A000000;
    if (!info->ptr || !info->pitch || info->line >= 55) return;
    volatile unsigned int *fb = (volatile unsigned int *)(info->ptr);
    unsigned int stride = info->pitch / 4;
    unsigned int y = info->line * 16;
    unsigned int sc = 2;
    unsigned int x = 10;

    /* Stage: 2 hex digits in green */
    _diag_hex(fb, stride, x, y, stage, 2, 0x00FF00, sc);
    x += 2 * 4 * sc + 3 * sc;

    /* Value 1: 8 hex digits in white */
    _diag_hex(fb, stride, x, y, v1, 8, 0xFFFFFF, sc);
    x += 8 * 4 * sc + 3 * sc;

    /* Value 2: 8 hex digits in cyan */
    _diag_hex(fb, stride, x, y, v2, 8, 0xFFFF00, sc);

    info->line++;
}

static inline void diag_bar(int width, unsigned int color) {
    volatile struct { unsigned long ptr; unsigned int pitch, width, height, line; }
        *info = (void *)0x3A000000;
    if (!info->ptr || !info->pitch || info->line >= 55) return;
    volatile unsigned int *fb = (volatile unsigned int *)(info->ptr);
    unsigned int stride = info->pitch / 4;
    unsigned int y = info->line * 16;
    for (unsigned int dy = 0; dy < 12; dy++)
        for (unsigned int dx = 0; dx < (unsigned int)width; dx++)
            fb[(y + dy) * stride + 10 + dx] = color;
    info->line++;
}

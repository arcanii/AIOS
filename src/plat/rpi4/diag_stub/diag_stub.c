/* Minimal framebuffer setup for RPi4 diagnostic display */
typedef unsigned int u32;
typedef unsigned long u64;
typedef volatile u32 vu32;

#define PERI_BASE    0xFE000000UL
#define GPIO_BASE    (PERI_BASE + 0x200000)
#define MBOX_BASE    (PERI_BASE + 0x00B880)

static inline void mmio_w(u64 base, u32 off, u32 val) { *(vu32*)(base+off) = val; }
static inline u32 mmio_r(u64 base, u32 off) { return *(vu32*)(base+off); }

static volatile u32 __attribute__((aligned(16))) mbox[36];
u32 fb_width, fb_height, fb_pitch;
u32 *fb_ptr;

int fb_init(u32 w, u32 h) {
    int i = 0;
    mbox[i++] = 35*4; mbox[i++] = 0;
    mbox[i++] = 0x48003; mbox[i++] = 8; mbox[i++] = 0; mbox[i++] = w; mbox[i++] = h;
    mbox[i++] = 0x48004; mbox[i++] = 8; mbox[i++] = 0; mbox[i++] = w; mbox[i++] = h;
    mbox[i++] = 0x48009; mbox[i++] = 8; mbox[i++] = 0; mbox[i++] = 0; mbox[i++] = 0;
    mbox[i++] = 0x48005; mbox[i++] = 4; mbox[i++] = 0; mbox[i++] = 32;
    mbox[i++] = 0x48006; mbox[i++] = 4; mbox[i++] = 0; mbox[i++] = 1;
    mbox[i++] = 0x40001; mbox[i++] = 8; mbox[i++] = 0; mbox[i++] = 4096; mbox[i++] = 0;
    mbox[i++] = 0x40008; mbox[i++] = 4; mbox[i++] = 0; mbox[i++] = 0;
    mbox[i++] = 0;

    u32 addr = (u32)(u64)&mbox | 8;
    while (mmio_r(MBOX_BASE, 0x38) & 0x80000000U) {}
    mmio_w(MBOX_BASE, 0x20, addr);
    while (1) {
        while (mmio_r(MBOX_BASE, 0x18) & 0x40000000U) {}
        if (mmio_r(MBOX_BASE, 0x00) == addr) break;
    }
    if (mbox[1] != 0x80000000U || mbox[28] == 0) return -1;
    fb_width = w; fb_height = h; fb_pitch = mbox[33];
    fb_ptr = (u32*)(u64)(mbox[28] & 0x3FFFFFFFU);
    return 0;
}

void fb_fill(u32 color) {
    u32 total = (fb_pitch/4) * fb_height;
    for (u32 i = 0; i < total; i++) fb_ptr[i] = color;
}

static const u32 hfont[16][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},
    {5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},
    {7,5,7,5,7},{7,5,7,1,7},{7,5,7,5,5},{6,5,7,5,6},
    {7,4,4,4,7},{6,5,5,5,6},{7,4,7,4,7},{7,4,7,4,4},
};

void fb_hex_digit(u32 x, u32 y, u32 d, u32 color, u32 sc) {
    if (d > 15) d = 0;
    u32 stride = fb_pitch / 4;
    for (u32 r = 0; r < 5; r++)
        for (u32 c = 0; c < 3; c++)
            if (hfont[d][r] & (4 >> c))
                for (u32 sy = 0; sy < sc; sy++)
                    for (u32 sx = 0; sx < sc; sx++)
                        fb_ptr[(y+r*sc+sy)*stride+(x+c*sc+sx)] = color;
}

void fb_hex32(u32 x, u32 y, u32 val, u32 color, u32 sc) {
    for (int i = 7; i >= 0; i--)
        fb_hex_digit(x + (u32)(7-i)*(4*sc), y, (val >> (i*4)) & 0xF, color, sc);
}

void fb_text(u32 x, u32 y, const char *s, u32 color, u32 sc) {
    while (*s) {
        char ch = *s++;
        u32 d = 0;
        if (ch >= '0' && ch <= '9') d = (u32)(ch - '0');
        else if (ch >= 'A' && ch <= 'F') d = (u32)(ch - 'A' + 10);
        else if (ch >= 'a' && ch <= 'f') d = (u32)(ch - 'a' + 10);
        else if (ch == ' ') { x += 4*sc; continue; }
        else if (ch == ':') { x += 2*sc; continue; }
        else { x += 4*sc; continue; }
        fb_hex_digit(x, y, d, color, sc);
        x += 4*sc;
    }
}

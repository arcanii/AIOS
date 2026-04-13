/*
 * fb_test.c -- RPi4 bare-metal framebuffer + LED test
 *
 * Standalone kernel (no seL4) that proves the boot chain works:
 *   1. Blinks ACT LED (GPIO 42)
 *   2. Allocates framebuffer via VideoCore mailbox
 *   3. Draws "AIOS RPi4 OK" on HDMI
 *
 * Build:
 *   aarch64-linux-gnu-gcc -nostdlib -nostartfiles -ffreestanding \
 *     -Wl,--section-start=.text=0x80000 -O2 -o fb_test.elf fb_test.c
 *   aarch64-linux-gnu-objcopy -O binary fb_test.elf kernel8.img
 */

/* BCM2711 peripheral base (ARM physical) */
#define PERI_BASE    0xFE000000UL
#define GPIO_BASE    (PERI_BASE + 0x200000)
#define MBOX_BASE    (PERI_BASE + 0x00B880)

/* GPIO registers */
#define GPFSEL4      0x10
#define GPSET1       0x20
#define GPCLR1       0x2C

/* Mailbox registers (offsets from MBOX_BASE) */
#define MBOX_READ    0x00
#define MBOX_STATUS  0x18
#define MBOX_WRITE   0x20
#define MBOX_FULL    0x80000000U
#define MBOX_EMPTY   0x40000000U
#define MBOX_CH_PROP 8

typedef unsigned int      u32;
typedef unsigned long     u64;
typedef volatile u32      vu32;

static inline void mmio_w(u64 base, u32 off, u32 val) {
    *(vu32 *)(base + off) = val;
}
static inline u32 mmio_r(u64 base, u32 off) {
    return *(vu32 *)(base + off);
}
static inline void delay(u32 n) {
    for (volatile u32 i = 0; i < n; i++) { __asm__ volatile("nop"); }
}

/* ---- LED ---- */
static void led_init(void) {
    u32 val = mmio_r(GPIO_BASE, GPFSEL4);
    val &= ~(7U << 6);  /* GPIO 42: bits 8:6 */
    val |= (1U << 6);   /* output */
    mmio_w(GPIO_BASE, GPFSEL4, val);
}
static void led_on(void)  { mmio_w(GPIO_BASE, GPSET1, 1U << 10); }
static void led_off(void) { mmio_w(GPIO_BASE, GPCLR1, 1U << 10); }

/* ---- Mailbox ---- */
static volatile u32 __attribute__((aligned(16))) mbox[36];

static int mbox_call(void) {
    u32 addr = (u32)(u64)&mbox;
    addr = (addr & ~0xFU) | MBOX_CH_PROP;

    while (mmio_r(MBOX_BASE, 0x38) & MBOX_FULL) {}
    mmio_w(MBOX_BASE, MBOX_WRITE, addr);

    while (1) {
        while (mmio_r(MBOX_BASE, MBOX_STATUS) & MBOX_EMPTY) {}
        if (mmio_r(MBOX_BASE, MBOX_READ) == addr)
            return (mbox[1] == 0x80000000U);
    }
}

/* ---- Framebuffer ---- */
static u32 fb_width, fb_height, fb_pitch;
static u32 *fb_ptr;

static int fb_init(u32 w, u32 h) {
    int i = 0;
    mbox[i++] = 35 * 4;       /* buffer size */
    mbox[i++] = 0;             /* request */

    mbox[i++] = 0x48003;       /* set physical w/h */
    mbox[i++] = 8; mbox[i++] = 0;
    mbox[i++] = w; mbox[i++] = h;

    mbox[i++] = 0x48004;       /* set virtual w/h */
    mbox[i++] = 8; mbox[i++] = 0;
    mbox[i++] = w; mbox[i++] = h;

    mbox[i++] = 0x48009;       /* set virtual offset */
    mbox[i++] = 8; mbox[i++] = 0;
    mbox[i++] = 0; mbox[i++] = 0;

    mbox[i++] = 0x48005;       /* set depth */
    mbox[i++] = 4; mbox[i++] = 0;
    mbox[i++] = 32;

    mbox[i++] = 0x48006;       /* set pixel order (RGB) */
    mbox[i++] = 4; mbox[i++] = 0;
    mbox[i++] = 1;

    mbox[i++] = 0x40001;       /* allocate buffer */
    mbox[i++] = 8; mbox[i++] = 0;
    mbox[i++] = 4096;          /* alignment */
    mbox[i++] = 0;

    mbox[i++] = 0x40008;       /* get pitch */
    mbox[i++] = 4; mbox[i++] = 0;
    mbox[i++] = 0;

    mbox[i++] = 0;             /* end tag */

    if (!mbox_call() || mbox[28] == 0) return -1;

    fb_width  = w;
    fb_height = h;
    fb_pitch  = mbox[33];
    /* Convert VC bus address to ARM physical */
    fb_ptr = (u32 *)(u64)(mbox[28] & 0x3FFFFFFFU);
    return 0;
}

static void fb_pixel(u32 x, u32 y, u32 color) {
    if (x < fb_width && y < fb_height)
        fb_ptr[y * (fb_pitch / 4) + x] = color;
}

static void fb_fill(u32 color) {
    u32 total = (fb_pitch / 4) * fb_height;
    for (u32 i = 0; i < total; i++) fb_ptr[i] = color;
}

static void fb_rect(u32 x, u32 y, u32 w, u32 h, u32 color) {
    for (u32 dy = 0; dy < h; dy++)
        for (u32 dx = 0; dx < w; dx++)
            fb_pixel(x + dx, y + dy, color);
}

/* ---- 8x8 bitmap font (uppercase + digits) ---- */
static const u64 font[] = {
    /* space */  0x0000000000000000ULL,
    /* A */      0x3C42427E424242ULL,
    /* B */      0x7C42427C42427CLL,
    /* C */      0x3C42404040423CLL,
    /* D */      0x7C42424242427CLL,
    /* E */      0x7E40407C40407ELL,
    /* F */      0x7E40407C404040LL,
    /* G */      0x3C4240404E423CLL,
    /* H */      0x4242427E424242LL,
    /* I */      0x3E08080808083ELL,
    /* J */      0x1E020202024224LL,
    /* K */      0x4244485060484442LL,
    /* L */      0x404040404040407ELL,
    /* M */      0x42665A5A424242ULL,
    /* N */      0x4262524A464242ULL,
    /* O */      0x3C42424242423CLL,
    /* P */      0x7C42427C404040LL,
    /* Q */      0x3C4242424A443AULL,
    /* R */      0x7C42427C484442LL,
    /* S */      0x3C42403C02423CLL,
    /* T */      0x7F08080808080808LL,
    /* U */      0x42424242424224LL,
    /* V */      0x4242424224241818LL,
    /* W */      0x42424242425A66ULL,
    /* X */      0x4242241818244242LL,
    /* Y */      0x4141221408080808LL,
    /* Z */      0x7E02040810207ELL,
    /* 0 */      0x3C46424242623CLL,
    /* 1 */      0x1838080808083ELL,
    /* 2 */      0x3C42020C30407ELL,
    /* 3 */      0x3C42021C02423CLL,
    /* 4 */      0x84488507E0808LL,
    /* 5 */      0x7E40407C02423CLL,
    /* 6 */      0x3C40407C42423CLL,
    /* 7 */      0x7E020408101010LL,
    /* 8 */      0x3C42423C42423CLL,
    /* 9 */      0x3C42423E02423CLL,
    /* ! */      0x1818181818001800LL,
    /* . */      0x0000000000001818LL,
    /* : */      0x0018180000181800LL,
};

static int char_index(char c) {
    if (c == ' ') return 0;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
    if (c >= 'a' && c <= 'z') return c - 'a' + 1;
    if (c >= '0' && c <= '9') return c - '0' + 27;
    if (c == '!') return 37;
    if (c == '.') return 38;
    if (c == ':') return 39;
    return 0;
}

static void fb_char(u32 x, u32 y, char c, u32 color, u32 scale) {
    u64 glyph = font[char_index(c)];
    for (u32 row = 0; row < 8; row++) {
        u32 bits = (glyph >> (row * 8)) & 0xFF;
        for (u32 col = 0; col < 8; col++) {
            if (bits & (1 << (7 - col))) {
                fb_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void fb_string(u32 x, u32 y, const char *s, u32 color, u32 scale) {
    while (*s) {
        fb_char(x, y, *s, color, scale);
        x += 8 * scale + scale;
        s++;
    }
}

/* ---- Entry point ---- */
void _start(void) {
    led_init();
    led_on();

    if (fb_init(1024, 768) == 0) {
        /* Dark blue background */
        fb_fill(0x001B2B);

        /* White banner */
        fb_rect(0, 0, 1024, 80, 0x1A1A2E);
        fb_string(40, 20, "AIOS RPI4 BOOT OK", 0x00FF88, 5);

        /* Status lines */
        fb_string(40, 120, "CPU: CORTEX A72", 0xCCCCCC, 3);
        fb_string(40, 160, "PLATFORM: BCM2711", 0xCCCCCC, 3);
        fb_string(40, 200, "FRAMEBUFFER: 1024X768", 0xCCCCCC, 3);
        fb_string(40, 260, "NEXT: CONNECT SERIAL", 0xFFAA00, 3);
        fb_string(40, 300, "GPIO 14 15 115200", 0xFFAA00, 3);

        /* Footer */
        fb_string(40, 700, "SEL4 AARCH64 MICROKERNEL OS", 0x666666, 2);
    }

    /* Blink LED forever */
    while (1) {
        led_on();
        delay(2000000);
        led_off();
        delay(2000000);
    }
}

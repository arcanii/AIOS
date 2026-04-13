/*
 * diag_main.c -- RPi4 diagnostic stub main
 *
 * Inits framebuffer, displays build ID and boot info,
 * then disables MMU/caches and jumps to seL4 elfloader.
 */
extern unsigned int *fb_ptr;
extern unsigned int fb_pitch, fb_width, fb_height;
extern int fb_init(unsigned int w, unsigned int h);
extern void fb_fill(unsigned int color);
extern void fb_hex32(unsigned int x, unsigned int y, unsigned int val,
                     unsigned int color, unsigned int sc);

struct fb_info {
    unsigned long ptr;
    unsigned int pitch;
    unsigned int width;
    unsigned int height;
    unsigned int line;
};

void diag_main(void) {
    unsigned long r0, r1, r2, r3;
    asm volatile("mov %0, x20" : "=r"(r0));
    asm volatile("mov %0, x21" : "=r"(r1));
    asm volatile("mov %0, x22" : "=r"(r2));
    asm volatile("mov %0, x23" : "=r"(r3));

    unsigned int el;
    asm volatile("mrs %0, CurrentEL" : "=r"(el));
    el = (el >> 2) & 3;

    /* Read build ID from offset 0xFFC (written by python combiner) */
    unsigned int build_id;
    asm volatile("adr x9, _start\n add x9, x9, #0xFFC\n ldr %w0, [x9]"
                 : "=r"(build_id) :: "x9");

    int ret = fb_init(1024, 768);
    if (ret == 0) {
        fb_fill(0x101010);  /* near-black background */

        /* Store framebuffer info at phys 0x1000 for elfloader diag_bar() */
        volatile struct fb_info *info = (volatile struct fb_info *)0x3A000000;
        info->ptr = (unsigned long)(fb_ptr);
        info->pitch = fb_pitch;
        info->width = 1024;
        info->height = 768;
        info->line = 8;

        /* Line 1: build verification code (green) */
        fb_hex32(10, 10, build_id, 0xFFFFFF, 4);

        /* Line 2: exception level + DTB address */
        fb_hex32(10, 50, el, 0x00FF88, 3);
        fb_hex32(200, 50, (unsigned int)(r0), 0xCCCCCC, 3);
    }

    /*
     * Disable MMU + D-cache + I-cache before jumping to seL4.
     *
     * RPi4 firmware leaves MMU on with page tables that protect low
     * physical memory (spin table region). The seL4 elfloader memset
     * to dest_paddr (~0x5000) faults without this.
     */
    asm volatile(
        "dsb sy\n"
        "mrs x0, sctlr_el2\n"
        "bic x0, x0, #1\n"        /* bit 0: M = MMU off */
        "bic x0, x0, #4\n"        /* bit 2: C = D-cache off */
        "bic x0, x0, #0x1000\n"   /* bit 12: I = I-cache off */
        "msr sctlr_el2, x0\n"
        "isb\n"
        "ic  iallu\n"              /* invalidate all I-cache */
        "tlbi alle2\n"            /* invalidate all TLB at EL2 */
        "dsb sy\n"
        "isb\n"
        ::: "x0", "memory"
    );

    /* Jump to seL4 elfloader at stub + 0x1000 */
    asm volatile(
        "mov x0, %0\n"
        "mov x1, %1\n"
        "mov x2, %2\n"
        "mov x3, %3\n"
        "adr x9, _start\n"
        "add x9, x9, #0x1000\n"
        "br x9\n"
        :: "r"(r0), "r"(r1), "r"(r2), "r"(r3)
        : "x0","x1","x2","x3","x9"
    );
}

/*
 * display_server.c -- Framebuffer display server
 *
 * IPC server for user programs to draw on the ramfb framebuffer.
 * Runs in the root task with direct access to gpu_fb[].
 */
#include "aios/root_shared.h"
#include "aios/gpu.h"
#include "aios/vfs.h"
#include "aios/fb_console.h"
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

/* ============================================================
 * Software 3D: spinning wireframe cube (FP-free, fixed point).
 * Bhaskara I integer sine (Q10) + perspective projection +
 * Bresenham lines straight to gpu_fb. A CPU 3D demo on the
 * framebuffer -- no GPU acceleration (see docs/DESIGN_RPI4_3D.md
 * for how hardware V3D 3D would be done).
 * ============================================================ */
static int gfx_isin(int a){ a%=360; if(a<0)a+=360; int s=1; if(a>180){a-=180;s=-1;}
    int t=a*(180-a); return s*(4*t*1024)/(40500-t); }
static int gfx_icos(int a){ return gfx_isin(a+90); }

static void gfx_put(int x,int y,uint32_t c){
    if((unsigned)x<gpu_width && (unsigned)y<gpu_height) gpu_fb[y*gpu_width+x]=c; }
static void gfx_line(int x0,int y0,int x1,int y1,uint32_t c){
    int dx=x1-x0,dy=y1-y0; int ax=dx<0?-dx:dx,ay=dy<0?-dy:dy; int sx=dx<0?-1:1,sy=dy<0?-1:1;
    int err=(ax>ay?ax:-ay)/2,e2;
    for(;;){ gfx_put(x0,y0,c); if(x0==x1&&y0==y1)break; e2=err;
        if(e2>-ax){err-=ay;x0+=sx;} if(e2<ay){err+=ax;y0+=sy;} } }

static const int GFX_CV[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                               {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
static const int GFX_CE[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},
                                {0,4},{1,5},{2,6},{3,7}};

static void gfx_render_cube(int ax,int ay){
    uint32_t bg=GPU_PIXEL(12,14,32), total=gpu_width*gpu_height;
    for(uint32_t i=0;i<total;i++) gpu_fb[i]=bg;
    int cx=(int)gpu_width/2, cy=(int)gpu_height/2;
    int scale=((int)gpu_height*2)/5, dist=3;
    int sa=gfx_isin(ay),ca=gfx_icos(ay),sb=gfx_isin(ax),cb=gfx_icos(ax);
    int sx[8],sy[8];
    for(int i=0;i<8;i++){
        int X=GFX_CV[i][0]*1024,Y=GFX_CV[i][1]*1024,Z=GFX_CV[i][2]*1024;   /* Q10 */
        int Xr=(X*ca+Z*sa)>>10;
        int Zr=(-X*sa+Z*ca)>>10;
        int Yr=(Y*cb-Zr*sb)>>10;
        int Zr2=(Y*sb+Zr*cb)>>10;
        int zc=Zr2+dist*1024; if(zc<16)zc=16;
        sx[i]=cx+(Xr*scale)/zc;
        sy[i]=cy-(Yr*scale)/zc;
    }
    uint32_t col=GPU_PIXEL(120,200,255);
    for(int e=0;e<12;e++)
        gfx_line(sx[GFX_CE[e][0]],sy[GFX_CE[e][0]],sx[GFX_CE[e][1]],sy[GFX_CE[e][1]],col);
}

static void gfx_demo_cube(void){
    for(int t=0;t<200;t++){
        gfx_render_cube((t*2)%360,(t*3)%360);
        for(volatile int d=0; d<1500000; d++) __asm__ volatile("":::"memory");
    }
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

        case DISP_CONSOLE: {
            /* Mirror tty output to the scrolling HDMI fb_console -- the boot console
             * continues straight into the interactive shell. len chars are packed
             * 8-per-word from MR1; fb_console_putc handles control chars + scrolling. */
            int len = (int)seL4_GetMR(0);
            if (len > 127) len = 127;
            for (int i = 0; i < len; i++) {
                seL4_Word w = seL4_GetMR(1 + i / 8);
                fb_console_putc((char)((w >> ((i % 8) * 8)) & 0xFF));
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }

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

        case DISP_CUBE: {
            if (!gpu_available || !gpu_fb) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            gfx_demo_cube();
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

/* cat.c — display file contents
 * Usage: pass filename via aios_get_arg(0)
 */
#include "aios_api.h"

int main(void) {
    const char *fname = aios_get_arg(0);
    if (!fname) {
        aios_puts("Usage: cat <file>\n");
        return 1;
    }
    int fd = aios_open(fname);
    if (fd < 0) {
        aios_puts("Error: file not found\n");
        return 1;
    }
    char buf[512];
    int n;
    while ((n = aios_read(fd, buf, 512)) > 0) {
        for (int i = 0; i < n; i++)
            aios_putc(buf[i]);
    }
    aios_close(fd);
    return 0;
}

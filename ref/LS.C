/* ls.c — list directory entries
 * AIOS sandboxed program
 * Uses: aios_opendir(), aios_readdir(), aios_puts()
 */
#include "aios_api.h"

int main(void) {
    DIR d;
    struct dirent ent;
    if (aios_opendir("/", &d) != 0) {
        aios_puts("Error: cannot open /\n");
        return 1;
    }
    while (aios_readdir(&d, &ent) == 0) {
        aios_puts(ent.name);
        aios_puts("  ");
        aios_put_dec(ent.size);
        aios_puts(" bytes\n");
    }
    return 0;
}

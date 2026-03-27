/* hello.c — simplest AIOS program
 * Prints a greeting to the console.
 */
#include "aios_api.h"

int main(void) {
    aios_puts("Hello from a sandboxed AIOS program!\n");
    return 0;
}

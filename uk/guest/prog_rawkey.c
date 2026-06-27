/* prog_rawkey.c -- proves RAW terminal mode works. It puts stdin in raw mode (cfmakeraw clears
 * ICANON/ECHO/...), then reads ONE byte: in raw mode read returns as soon as a key is pressed, with
 * no echo and no waiting for Enter -- in the default canonical mode it would block until a newline.
 * The rawkey_pty test sends a single byte (no newline); seeing "rawkey got:" proves raw mode. The
 * original attributes are restored before exit. */

#include <termios.h>
#include <unistd.h>
#include <stdio.h>

int
main(void)
{
    struct termios orig, raw;

    if (tcgetattr(0, &orig) != 0) {
        printf("rawkey: stdin is not a tty\n");
        return 2;
    }
    raw = orig;
    cfmakeraw(&raw);
    if (tcsetattr(0, TCSANOW, &raw) != 0) {
        printf("rawkey: tcsetattr(raw) failed\n");
        return 1;
    }

    char c = 0;
    long n = read(0, &c, 1);            /* raw: returns on the first keypress, unechoed */

    tcsetattr(0, TCSANOW, &orig);       /* restore canonical mode before printing */

    if (n == 1) {
        printf("rawkey got: %c\n", c);
        return 0;
    }
    printf("rawkey: read returned %ld\n", n);
    return 1;
}

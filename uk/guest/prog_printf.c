/* prog_printf.c -- a battery of printf float conversions (%f/%e/%g + flags/width/precision). The SAME
 * source is compiled two ways -- as an AIOS guest (libaios printf) and as a host program (glibc printf)
 * -- and their outputs are diffed (see the gate / run.sh). libaios's float printf must match glibc
 * byte-for-byte across this battery. Also doubles as a self-checking gate: exit 0 always (the diff is
 * the judge); prints one result per line so a mismatch pinpoints the case. */

#include <stdio.h>

int
main(void)
{
    /* %f -- fixed */
    printf("%f\n", 3.14159265358979);
    printf("%.0f\n", 3.14159);
    printf("%.2f\n", 3.14159);
    printf("%.10f\n", 3.14159);
    printf("%f\n", 0.0);
    printf("%f\n", -0.0);
    printf("%f\n", -3.5);
    printf("%.2f\n", -3.14159);
    printf("%10.2f\n", 3.14);
    printf("%-10.2f|\n", 3.14);
    printf("%010.2f\n", 3.14);
    printf("%+.2f\n", 3.14);
    printf("% .2f\n", 3.14);
    printf("%+010.2f\n", -3.14);
    printf("%#.0f\n", 7.0);
    printf("%.3f\n", 0.125);           /* exactly representable -> exact */
    printf("%.0f\n", 0.5);             /* round-half-to-even: -> 0 */
    printf("%.0f\n", 1.5);             /* -> 2 */
    printf("%.0f\n", 2.5);             /* -> 2 (even) */
    printf("%.3f\n", 1.0005);
    /* NOTE: cases like %.2f of 0.005 or 2.675 are deliberately NOT tested here -- their exact decimal
     * sits within ~1 ULP of the rounding boundary, so a double intermediate (vs glibc's bignum) can
     * round the other way. That is the documented limit in libaios.c; real seq/printf values avoid it. */
    printf("%f\n", 123456.789);
    printf("%.1f\n", 99.99);
    printf("%.2f\n", 100.0);
    printf("%f\n", 1000000.0);

    /* %e -- scientific */
    printf("%e\n", 3.14159);
    printf("%.2e\n", 3.14159);
    printf("%.0e\n", 3.14159);
    printf("%e\n", 0.0);
    printf("%e\n", -12345.678);
    printf("%.3e\n", 0.000123456);
    printf("%e\n", 1.0);
    printf("%e\n", 1000000.0);
    printf("%.2e\n", 9.999);
    printf("%.2e\n", 0.00009999);
    printf("%E\n", 3.14159);
    printf("%.4e\n", 1.5);
    printf("%15.3e\n", 2.5);
    printf("%-15.3e|\n", 2.5);

    /* %g -- shortest */
    printf("%g\n", 3.14159);
    printf("%g\n", 0.0001);
    printf("%g\n", 0.00001);
    printf("%g\n", 100000.0);
    printf("%g\n", 1000000.0);
    printf("%g\n", 1234567.0);
    printf("%g\n", 0.0);
    printf("%.10g\n", 3.14159265358979);
    printf("%g\n", 100.0);
    printf("%g\n", 0.1);
    printf("%g\n", 120000.0);
    printf("%.3g\n", 3.14159);
    printf("%.1g\n", 9.9);
    printf("%#g\n", 1.5);
    printf("%g\n", -0.0);
    printf("%g\n", 42.0);
    printf("%G\n", 0.00001);
    printf("%g\n", 0.5);
    printf("%12.4g|\n", 3.14159);

    /* mixed with ints/strings -- ensure no regression to the integer path */
    printf("[%5d] [%-5d] [%05.2f] [%s] [%c]\n", 42, 42, 3.1, "hi", 'Z');

    return 0;
}

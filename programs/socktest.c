#include "aios.h"
#include "posix.h"

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;

    printf("=== Socket API Test ===\n");

    /* Test 1: Create TCP socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    printf("1. socket(TCP) = %d %s\n", fd, fd >= 0 ? "OK" : "FAIL");

    /* Test 2: Create UDP socket */
    int fd2 = socket(AF_INET, SOCK_DGRAM, 0);
    printf("2. socket(UDP) = %d %s\n", fd2, fd2 >= 0 ? "OK" : "FAIL");

    /* Test 3: Bind */
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = INADDR_ANY;
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    printf("3. bind(8080) = %d %s\n", rc, rc == 0 ? "OK" : "FAIL");

    /* Test 4: Listen */
    rc = listen(fd, 5);
    printf("4. listen() = %d %s\n", rc, rc == 0 ? "OK" : "FAIL");

    /* Test 5: errno */
    printf("5. errno = %d (should be 0)\n", errno);

    /* Test 6: getenv */
    char *home = getenv("HOME");
    printf("6. HOME = %s\n", home ? home : "NULL");
    char *user = getenv("USER");
    printf("   USER = %s\n", user ? user : "NULL");
    char *term = getenv("TERM");
    printf("   TERM = %s\n", term ? term : "NULL");

    /* Test 7: setenv */
    setenv("FOO", "bar", 1);
    char *foo = getenv("FOO");
    printf("7. FOO = %s %s\n", foo ? foo : "NULL", (foo && foo[0] == 'b') ? "OK" : "FAIL");

    /* Test 8: htons/ntohs */
    unsigned short p = htons(8080);
    printf("8. htons(8080) = 0x%x, ntohs back = %d %s\n", 
           (unsigned int)p, (int)ntohs(p), ntohs(p) == 8080 ? "OK" : "FAIL");

    printf("=== All tests done ===\n");
    return 0;
}

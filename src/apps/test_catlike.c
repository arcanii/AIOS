/* test_catlike.c -- mimic cat behavior exactly */
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char buf[1024];
    int fd, n;
    const char *msg1 = "open...\n";
    const char *msg2 = "read...\n";
    const char *msg3 = "write...\n";
    const char *msg4 = "done\n";

    (void)argc; (void)argv;

    write(1, msg1, 8);
    fd = open("/tmp/hello.c", 0);
    write(1, msg2, 8);
    n = read(fd, buf, 1024);
    write(1, msg3, 9);
    write(1, buf, n);
    write(1, msg4, 5);
    close(fd);
    return 0;
}

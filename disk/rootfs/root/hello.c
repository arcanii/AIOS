#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
int main() {
    int fd = open("/tmp/proof", 65, 0644);
    write(fd, "TCC_OK\n", 7);
    close(fd);
    printf("Hello from tcc!\n");
    write(1, "Direct OK\n", 10);
    return 0;
}

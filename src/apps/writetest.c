#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    const char msg[] = "WRITETEST: direct write\n";
    write(1, msg, sizeof(msg) - 1);
    printf("WRITETEST: printf works\n");
    return 0;
}

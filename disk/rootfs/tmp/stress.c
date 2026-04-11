#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main() {
    char buf[256];
    sprintf(buf, "sum=%d", 1+2+3);
    printf("%s len=%d\n", buf, (int)strlen(buf));
    return 0;
}

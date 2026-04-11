#include <stdio.h>
extern int add(int, int);
extern int mul(int, int);
int main() {
    printf("add(3,4)=%d\n", add(3, 4));
    printf("mul(5,6)=%d\n", mul(5, 6));
    return 0;
}

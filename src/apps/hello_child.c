/*
 * AIOS 0.4.x — first child process
 */
#include <stdio.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[]) {
    /* Use seL4_DebugPutChar directly — printf may not be wired */
    seL4_DebugPutChar('\n');
    seL4_DebugPutChar('[');
    seL4_DebugPutChar('c');
    seL4_DebugPutChar('h');
    seL4_DebugPutChar('i');
    seL4_DebugPutChar('l');
    seL4_DebugPutChar('d');
    seL4_DebugPutChar(']');
    seL4_DebugPutChar(' ');
    seL4_DebugPutChar('H');
    seL4_DebugPutChar('e');
    seL4_DebugPutChar('l');
    seL4_DebugPutChar('l');
    seL4_DebugPutChar('o');
    seL4_DebugPutChar('!');
    seL4_DebugPutChar('\n');

    printf("[child] printf works too!\n");
    printf("[child] I have my own VSpace + TCB!\n");
    printf("[child] Exiting with code 42.\n");

    return 42;
}

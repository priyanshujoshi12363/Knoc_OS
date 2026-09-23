#include "ulib.h"

/* Tries things it is not allowed to do. The kernel refuses the system
   call, the page table blocks the memory read, and the AI space sees a
   program that crashed after a forbidden call. */

int main(void)
{
    print("[spy] trying to start another program without permission\n");
    spawn("hello");

    print("[spy] trying to read kernel memory at 0x80000000\n");

    unsigned long value = *(volatile unsigned long *)0x80000000UL;

    print("[spy] read kernel memory!\n");
    return (int)value;
}

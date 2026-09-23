#include "ulib.h"

int main(void)
{
    print("[crash] writing to a null pointer\n");

    *(volatile int *)0 = 1;

    print("[crash] still alive?\n");
    return 0;
}

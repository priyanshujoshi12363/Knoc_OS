#include "ulib.h"

/* Passes bad arguments to system calls: each one must return an error,
   and the kernel must not crash. */

int main(void)
{
    long result = write(FD_STDOUT, (const void *)0x80000000UL, 16);

    if (result != E_FAULT)
    {
        print("[badcall] write from kernel memory was not refused\n");
        return 1;
    }

    result = write(FD_STDOUT, (const void *)0x1050000000UL, 16);

    if (result != E_FAULT)
    {
        print("[badcall] write from unmapped memory was not refused\n");
        return 2;
    }

    result = syscall(99, 0, 0, 0);

    if (result != E_BADCALL)
    {
        print("[badcall] unknown system call was not refused\n");
        return 3;
    }

    result = spawn((const char *)0x80001000UL);

    if (result != E_FAULT && result != E_PERM)
    {
        print("[badcall] spawn with a kernel pointer was not refused\n");
        return 4;
    }

    print("[badcall] kernel pointer, unmapped pointer and unknown call were all refused\n");
    return 0;
}

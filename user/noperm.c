#include "ulib.h"

int main(void)
{
    int result = spawn("hello");

    if (result != E_PERM)
    {
        print("[noperm] spawn without the SPAWN capability was not refused\n");
        return 1;
    }

    if (open("/hello.txt", O_READ) != E_PERM)
    {
        print("[noperm] open without the FILES_READ capability was not refused\n");
        return 2;
    }

    print("[noperm] spawn and open were refused: this program has no SPAWN or FILES capability\n");
    return 0;
}

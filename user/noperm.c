#include "ulib.h"

int main(void)
{
    int result = spawn("hello");

    if (result != E_PERM)
    {
        print("[noperm] spawn without the SPAWN capability was not refused\n");
        return 1;
    }

    print("[noperm] spawn was refused: this program has no SPAWN capability\n");
    return 0;
}

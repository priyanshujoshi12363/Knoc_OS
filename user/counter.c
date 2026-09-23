#include "ulib.h"

/* Counts forever, one line every half second: stop it with Ctrl-C */

int main(void)
{
    for (unsigned long i = 1;; i++)
    {
        print("[counter] ");
        print_uint(i);
        print("\n");
        sleep(50);
    }
}

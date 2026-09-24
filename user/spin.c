#include "ulib.h"

int main(void)
{
    volatile unsigned long counter = 0;

    while (1)
    {
        counter++;
    }
}

#include "ulib.h"

int main(void)
{
    char args[ARGS_MAX];
    long rate = 0;

    getargs(args, sizeof(args));
    rate = parse_number(args);

    if (rate <= 0)
    {
        rate = 256;
    }

    while (1)
    {
        unsigned char *block = mem_alloc((unsigned long)rate * 1024);

        if (block != 0)
        {
            memset(block, 1, (unsigned long)rate * 1024);
        }

        sleep(100);
    }
}

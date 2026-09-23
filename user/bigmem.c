#include "ulib.h"

/* Runs as AI_AGENT, which has a 1 GiB quota: room for model weights */

#define BLOCK_BYTES (256UL * 1024 * 1024)
#define STEP (2UL * 1024 * 1024)

int main(void)
{
    unsigned char *block = mem_alloc(BLOCK_BYTES);

    if (block == 0)
    {
        print("[bigmem] 256 MiB allocation failed\n");
        return 1;
    }

    for (unsigned long offset = 0; offset < BLOCK_BYTES; offset += STEP)
    {
        *(volatile unsigned long *)(block + offset) = offset ^ 0xA5A5A5A5UL;
    }

    for (unsigned long offset = 0; offset < BLOCK_BYTES; offset += STEP)
    {
        if (*(volatile unsigned long *)(block + offset) != (offset ^ 0xA5A5A5A5UL))
        {
            print("[bigmem] memory check failed\n");
            return 2;
        }
    }

    print("[bigmem] AI agent got 256 MiB at ");
    print_hex((unsigned long)block);
    print(" and used all of it\n");
    return 0;
}

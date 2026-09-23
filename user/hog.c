#include "ulib.h"

/* A NORMAL program has a 16 MiB memory quota */

int main(void)
{
    if (mem_alloc(8UL * 1024 * 1024) == 0)
    {
        print("[hog] 8 MiB inside the quota failed\n");
        return 1;
    }

    if (mem_alloc(16UL * 1024 * 1024) != 0)
    {
        print("[hog] 16 MiB more was not refused\n");
        return 2;
    }

    print("[hog] 8 MiB allowed, 16 MiB more refused: quota is 16 MiB\n");
    return 0;
}

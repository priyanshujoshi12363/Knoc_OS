#include "ulib.h"

int main(void)
{
    print("[hello] Hello from user mode! pid ");
    print_uint((unsigned long)getpid());
    print(", uptime ticks ");
    print_uint(uptime());
    print("\n");

    unsigned long *memory = mem_alloc(4096);

    if (memory == 0)
    {
        print("[hello] mem_alloc failed\n");
        return 1;
    }

    for (int i = 0; i < 512; i++)
    {
        memory[i] = (unsigned long)i * 3;
    }

    for (int i = 0; i < 512; i++)
    {
        if (memory[i] != (unsigned long)i * 3)
        {
            print("[hello] memory check failed\n");
            return 2;
        }
    }

    yield();
    sleep(2);

    print("[hello] got 4 KiB at ");
    print_hex((unsigned long)memory);
    print(", slept and yielded, exiting\n");

    return 0;
}

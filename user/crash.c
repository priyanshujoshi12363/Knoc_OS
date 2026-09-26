#include "ulib.h"

typedef void (*function_t)(void);

static unsigned long seed = 1;

static unsigned long next_random(void)
{
    seed = seed * 6364136223846793005UL + 1442695040888963407UL;
    return seed >> 17;
}

static unsigned long between(unsigned long low, unsigned long high)
{
    return low + next_random() % (high - low);
}

static volatile unsigned long sink;
static volatile unsigned long depth_limit = 1000000000UL;

static void touch(unsigned long address)
{
    if (next_random() & 1)
    {
        *(volatile unsigned long *)address = 1;
    }
    else
    {
        sink = *(volatile unsigned long *)address;
    }
}

static unsigned long deep(unsigned long level)
{
    volatile char frame[512];

    if (level > depth_limit)
    {
        return 0;
    }

    frame[0] = (char)level;
    frame[511] = (char)level;
    return deep(level + 1) + frame[0] + frame[511];
}

static void outside_address(unsigned long *address)
{
    switch (next_random() % 4)
    {
    case 0:
        *address = between(0x80000000UL, 0x88000000UL) & ~7UL;
        break;
    case 1:
        *address = between(0x10000000UL, 0x10001000UL) & ~7UL;
        break;
    case 2:
        *address = between(0x2000000000UL, 0x3F00000000UL) & ~7UL;
        break;
    default:
        *address = between(0x100000UL, 0x8000000UL) & ~7UL;
        break;
    }
}

int main(void)
{
    char args[ARGS_MAX];
    char mode[16];
    int n = 0;

    getargs(args, sizeof(args));

    while (args[n] && args[n] != ' ' && n < (int)sizeof(mode) - 1)
    {
        mode[n] = args[n];
        n++;
    }

    mode[n] = 0;

    if (args[n] == ' ')
    {
        seed = (unsigned long)parse_number(args + n + 1) * 2654435761UL + 1;
    }

    unsigned long address;

    if (n == 0)
    {
        print("[crash] writing to a null pointer\n");
        *(volatile int *)0 = 1;
    }
    else if (strcmp(mode, "null") == 0)
    {
        address = between(0, 0x1000) & ~7UL;
        print("[crash] null pointer\n");
        touch(address);
    }
    else if (strcmp(mode, "wild") == 0)
    {
        outside_address(&address);
        print("[crash] pointer outside the program\n");
        touch(address);
    }
    else if (strcmp(mode, "unmapped") == 0)
    {
        address = (USER_HEAP_BASE + between(0x100000UL, 0x40000000UL)) & ~7UL;
        print("[crash] pointer to memory the program never allocated\n");
        touch(address);
    }
    else if (strcmp(mode, "stack") == 0)
    {
        print("[crash] endless recursion\n");
        sink = deep(0);
    }
    else if (strcmp(mode, "jump") == 0)
    {
        if (next_random() % 5 == 0)
        {
            address = 0;
        }
        else if (next_random() % 3 == 0)
        {
            address = (USER_HEAP_BASE + between(0x100000UL, 0x40000000UL)) & ~3UL;
        }
        else
        {
            outside_address(&address);
        }

        print("[crash] call through a broken function pointer\n");
        ((function_t)address)();
    }
    else if (strcmp(mode, "illegal") == 0)
    {
        print("[crash] illegal instruction\n");

        if (next_random() & 1)
        {
            asm volatile(".word 0x00000000");
        }
        else
        {
            unsigned long value;

            asm volatile("csrr %0, mstatus" : "=r"(value));
            sink = value;
        }
    }
    else if (strcmp(mode, "misaligned") == 0)
    {
        static unsigned long words[8];
        unsigned long base = (unsigned long)&words[2] + 1 + next_random() % 6;

        print("[crash] misaligned atomic access\n");

        if (next_random() & 1)
        {
            unsigned long old;

            asm volatile("amoswap.d %0, %1, (%2)" : "=r"(old) : "r"(1UL), "r"(base) : "memory");
            sink = old;
        }
        else
        {
            unsigned long value;

            asm volatile("lr.d %0, (%1)" : "=r"(value) : "r"(base) : "memory");
            sink = value;
        }
    }
    else
    {
        print("usage: crash [null | wild | unmapped | stack | jump | illegal | misaligned] [SEED]\n");
        return 1;
    }

    print("[crash] still alive?\n");
    return 0;
}

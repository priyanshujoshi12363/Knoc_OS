#include "program.h"
#include "syscall_abi.h"

#define PROGRAM(name) \
    extern const uint8_t program_##name##_start[]; \
    extern const uint8_t program_##name##_end[];

PROGRAM(hello)
PROGRAM(badcall)
PROGRAM(noperm)
PROGRAM(hog)
PROGRAM(bigmem)
PROGRAM(crash)
PROGRAM(spy)

#define ENTRY(name, class, caps) \
    {#name, program_##name##_start, program_##name##_end, class, caps}

static const program_t programs[] = {
    ENTRY(hello, PROCESS_CLASS_NORMAL, CAP_CONSOLE | CAP_SPAWN | CAP_MEMORY),
    ENTRY(badcall, PROCESS_CLASS_NORMAL, CAP_CONSOLE | CAP_SPAWN),
    ENTRY(noperm, PROCESS_CLASS_NORMAL, CAP_CONSOLE),
    ENTRY(hog, PROCESS_CLASS_NORMAL, CAP_CONSOLE | CAP_MEMORY),
    ENTRY(bigmem, PROCESS_CLASS_AI_AGENT, CAP_CONSOLE | CAP_MEMORY),
    ENTRY(crash, PROCESS_CLASS_NORMAL, CAP_CONSOLE),
    ENTRY(spy, PROCESS_CLASS_NORMAL, CAP_CONSOLE),
};

#define PROGRAM_COUNT (sizeof(programs) / sizeof(programs[0]))

static int names_equal(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }

    return *a == *b;
}

const program_t *program_find(const char *name)
{
    for (uint32_t i = 0; i < PROGRAM_COUNT; i++)
    {
        if (names_equal(programs[i].name, name))
        {
            return &programs[i];
        }
    }

    return 0;
}

uint32_t program_count(void)
{
    return PROGRAM_COUNT;
}

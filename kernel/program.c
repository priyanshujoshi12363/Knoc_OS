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
PROGRAM(files)
PROGRAM(modelcheck)
PROGRAM(knocsh)
PROGRAM(counter)
PROGRAM(organize)

#define ENTRY(name, class, caps, flags) \
    {#name, program_##name##_start, program_##name##_end, class, caps, flags}

static const program_t programs[] = {
    ENTRY(hello, PROCESS_CLASS_NORMAL, CAP_CONSOLE | CAP_SPAWN | CAP_MEMORY, 0),
    ENTRY(badcall, PROCESS_CLASS_NORMAL, CAP_CONSOLE | CAP_SPAWN, 0),
    ENTRY(noperm, PROCESS_CLASS_NORMAL, CAP_CONSOLE, 0),
    ENTRY(hog, PROCESS_CLASS_NORMAL, CAP_CONSOLE | CAP_MEMORY, 0),
    ENTRY(bigmem, PROCESS_CLASS_AI_AGENT, CAP_CONSOLE | CAP_MEMORY, 0),
    ENTRY(crash, PROCESS_CLASS_NORMAL, CAP_CONSOLE, 0),
    ENTRY(spy, PROCESS_CLASS_NORMAL, CAP_CONSOLE, 0),
    ENTRY(files, PROCESS_CLASS_NORMAL, CAP_CONSOLE | CAP_FILES_READ | CAP_FILES_WRITE, 0),
    ENTRY(modelcheck, PROCESS_CLASS_AI_AGENT, CAP_CONSOLE | CAP_FILES_READ | CAP_MEMORY, 0),
    ENTRY(knocsh, PROCESS_CLASS_INTERACTIVE,
          CAP_CONSOLE | CAP_SPAWN | CAP_MEMORY | CAP_FILES_READ | CAP_FILES_WRITE | CAP_SYSTEM | CAP_KNOWLEDGE,
          PROGRAM_TERMINAL),
    ENTRY(counter, PROCESS_CLASS_NORMAL, CAP_CONSOLE, 0),
    ENTRY(organize, PROCESS_CLASS_NORMAL, CAP_CONSOLE | CAP_MEMORY | CAP_FILES_READ | CAP_FILES_WRITE | CAP_KNOWLEDGE, 0),
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

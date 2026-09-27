#include "program.h"
#include "syscall_abi.h"
#include "knocfs.h"

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
PROGRAM(leak)
PROGRAM(spin)
PROGRAM(diskload)
PROGRAM(quiet)
PROGRAM(spawner)
PROGRAM(filler)
PROGRAM(recorder)
PROGRAM(healthd)
PROGRAM(ask)
PROGRAM(agent)
PROGRAM(chat)
PROGRAM(organized)

#define ENTRY(name, class, caps, flags) \
    {#name, program_##name##_start, program_##name##_end, class, caps, flags, 0}

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
    ENTRY(leak, PROCESS_CLASS_AI_AGENT, CAP_CONSOLE | CAP_MEMORY, 0),
    ENTRY(spin, PROCESS_CLASS_NORMAL, CAP_CONSOLE, 0),
    ENTRY(diskload, PROCESS_CLASS_NORMAL, CAP_CONSOLE | CAP_FILES_READ | CAP_FILES_WRITE, 0),
    ENTRY(quiet, PROCESS_CLASS_NORMAL, 0, 0),
    ENTRY(spawner, PROCESS_CLASS_NORMAL, CAP_CONSOLE | CAP_SPAWN, 0),
    ENTRY(filler, PROCESS_CLASS_NORMAL, CAP_CONSOLE | CAP_FILES_READ | CAP_FILES_WRITE, 0),
    ENTRY(recorder, PROCESS_CLASS_BACKGROUND, CAP_CONSOLE | CAP_SYSTEM, 0),
    ENTRY(healthd, PROCESS_CLASS_BACKGROUND,
          CAP_CONSOLE | CAP_SYSTEM | CAP_KNOWLEDGE | CAP_FILES_READ | CAP_MEMORY, 0),
    ENTRY(agent, PROCESS_CLASS_AI_AGENT,
          CAP_CONSOLE | CAP_SPAWN | CAP_SYSTEM | CAP_KNOWLEDGE | CAP_FILES_READ | CAP_FILES_WRITE | CAP_MEMORY, 0),
    ENTRY(organized, PROCESS_CLASS_BACKGROUND, CAP_CONSOLE | CAP_SPAWN | CAP_FILES_READ, 0),
    ENTRY(chat, PROCESS_CLASS_AI_AGENT,
          CAP_CONSOLE | CAP_SPAWN | CAP_SYSTEM | CAP_KNOWLEDGE | CAP_FILES_READ | CAP_FILES_WRITE | CAP_MEMORY, 0),
    ENTRY(ask, PROCESS_CLASS_AI_AGENT,
          CAP_CONSOLE | CAP_SYSTEM | CAP_KNOWLEDGE | CAP_FILES_READ | CAP_FILES_WRITE | CAP_MEMORY, 0),
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

#define INSTALLED_MAX 32
#define INSTALLED_CAPABILITIES (CAP_CONSOLE | CAP_FILES_READ | CAP_FILES_WRITE | CAP_MEMORY)

static program_t installed[INSTALLED_MAX];
static char installed_names[INSTALLED_MAX][PROCESS_NAME_MAX];
static char installed_paths[INSTALLED_MAX][PATH_MAX];
static uint32_t installed_count;

static int valid_name(const char *name)
{
    int length = 0;

    for (; name[length]; length++)
    {
        char c = name[length];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
              c == '_'))
        {
            return 0;
        }
    }

    return length > 0 && length < PROCESS_NAME_MAX;
}

static const program_t *install(const char *path)
{
    uint32_t inode;
    knocfs_stat_t stat;

    for (uint32_t i = 0; i < installed_count; i++)
    {
        if (names_equal(installed_paths[i], path))
        {
            return &installed[i];
        }
    }

    if (installed_count >= INSTALLED_MAX || !knocfs_mounted() || knocfs_lookup(path, &inode) != 0 ||
        knocfs_stat(inode, &stat) != 0 || stat.type != KNOCFS_TYPE_FILE)
    {
        return 0;
    }

    const char *base = path;

    for (int i = 0; path[i]; i++)
    {
        if (path[i] == '/' && path[i + 1])
        {
            base = path + i + 1;
        }
    }

    char *name = installed_names[installed_count];
    int n = 0;

    while (base[n] && n < PROCESS_NAME_MAX - 1)
    {
        name[n] = base[n];
        n++;
    }

    name[n] = 0;

    for (n = 0; path[n] && n < PATH_MAX - 1; n++)
    {
        installed_paths[installed_count][n] = path[n];
    }

    installed_paths[installed_count][n] = 0;

    program_t *program = &installed[installed_count];

    program->name = name;
    program->start = 0;
    program->end = 0;
    program->process_class = PROCESS_CLASS_NORMAL;
    program->capabilities = INSTALLED_CAPABILITIES | (path[0] == '/' && path[1] == 'b' && path[2] == 'i' && path[3] == 'n' && path[4] == '/' ? CAP_NET : 0);
    program->flags = 0;
    program->path = installed_paths[installed_count];

    if (names_equal(name, "indexd"))
    {
        program->process_class = PROCESS_CLASS_BACKGROUND;
    }

    if (names_equal(name, "knocnetd"))
    {
        program->process_class = PROCESS_CLASS_BACKGROUND;
        program->capabilities |= CAP_SPAWN | CAP_SYSTEM | CAP_KNOWLEDGE;
    }

    installed_count++;
    return program;
}

const program_t *program_installed(const char *name)
{
    char path[PROCESS_NAME_MAX + 8] = "/bin/";

    if (!valid_name(name))
    {
        return 0;
    }

    for (int i = 0; name[i]; i++)
    {
        path[5 + i] = name[i];
        path[6 + i] = 0;
    }

    return install(path);
}

const program_t *program_at_path(const char *path)
{
    return path[0] == '/' ? install(path) : 0;
}

uint32_t program_count(void)
{
    return PROGRAM_COUNT;
}

#include "linux.h"
#include "syscall.h"
#include "syscall_abi.h"
#include "process.h"
#include "knocfs.h"
#include "timer.h"
#include "rtc.h"
#include "virtio_rng.h"
#include "string.h"
#include "heap.h"
#include "tty.h"
#include "uart.h"
#include "page.h"
#include "vm.h"

#define FDS_MAX 32
#define AT_FDCWD -100
#define AT_REMOVEDIR 0x200
#define AT_EMPTY_PATH 0x1000
#define BRK_STEP (64UL * 1024)
#define LINE_MAX 512
#define DIRENT_BUFFER 4096
#define WARNED_MAX 64
#define PROC_TEXT_MAX 2048
#define PROC_PIDS_MAX 64

enum
{
    L_EPERM = 1,
    L_ENOENT = 2,
    L_ESRCH = 3,
    L_EINTR = 4,
    L_EIO = 5,
    L_EBADF = 9,
    L_ECHILD = 10,
    L_EAGAIN = 11,
    L_ENOMEM = 12,
    L_EACCES = 13,
    L_EFAULT = 14,
    L_EEXIST = 17,
    L_ENODEV = 19,
    L_ENOTDIR = 20,
    L_EISDIR = 21,
    L_EINVAL = 22,
    L_EMFILE = 24,
    L_ENOTTY = 25,
    L_ENOSPC = 28,
    L_ESPIPE = 29,
    L_ERANGE = 34,
    L_ENOSYS = 38,
    L_ENOTEMPTY = 39,
    L_ENETDOWN = 100,
    L_ETIMEDOUT = 110,
    L_ECONNREFUSED = 111,
};

enum
{
    FD_FREE,
    FD_CONSOLE_IN,
    FD_CONSOLE_OUT,
    FD_FILE,
    FD_DIR,
    FD_NULL,
    FD_MEMORY,
};

typedef struct linux_fd
{
    int kind;
    int knoc;
    int append;
    uint32_t access;
    uint32_t dir_index;
    char *path;
    char *data;
    uint32_t data_size;
    uint32_t data_offset;
} linux_fd_t;

typedef struct linux_state
{
    linux_fd_t fds[FDS_MAX];
    uintptr_t brk_start;
    uintptr_t brk_now;
    uintptr_t brk_mapped;
    uintptr_t mmap_next;
    uint8_t termios[36];
    char line[LINE_MAX];
    uint32_t line_length;
    uint32_t line_used;
    char exe[PATH_MAX];
    char name[PROCESS_NAME_MAX];
    uint16_t warned[WARNED_MAX];
    int warned_count;
} linux_state_t;

typedef struct linux_stat
{
    uint64_t dev;
    uint64_t ino;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t rdev;
    uint64_t pad1;
    int64_t size;
    int32_t blksize;
    int32_t pad2;
    int64_t blocks;
    int64_t atime;
    uint64_t atime_nsec;
    int64_t mtime;
    uint64_t mtime_nsec;
    int64_t ctime;
    uint64_t ctime_nsec;
    uint32_t unused[2];
} linux_stat_t;

static const char *environment[] = {"PATH=/bin", "HOME=/home", "TERM=vt100", "USER=root", "LOGNAME=root",
                                    "SHELL=/bin/sh", "LANG=C", "COLUMNS=80", "LINES=24"};

static linux_state_t *state(void)
{
    return (linux_state_t *)process_linux_state();
}

static int64_t error_of(int64_t knoc)
{
    switch (knoc)
    {
    case E_PERM:
        return -L_EACCES;
    case E_NOMEM:
        return -L_ENOMEM;
    case E_NOTFOUND:
        return -L_ENOENT;
    case E_INVAL:
        return -L_EINVAL;
    case E_EXISTS:
        return -L_EEXIST;
    case E_NOSPACE:
        return -L_ENOSPC;
    case E_BADF:
        return -L_EBADF;
    case E_ISDIR:
        return -L_EISDIR;
    case E_NOTDIR:
        return -L_ENOTDIR;
    case E_NOTEMPTY:
        return -L_ENOTEMPTY;
    case E_FAULT:
        return -L_EFAULT;
    case E_TIMEOUT:
        return -L_ETIMEDOUT;
    case E_REFUSED:
        return -L_ECONNREFUSED;
    case E_NETDOWN:
        return -L_ENETDOWN;
    case E_NODEV:
        return -L_ENODEV;
    case E_BADCALL:
        return -L_ENOSYS;
    default:
        return knoc < 0 ? -L_EIO : knoc;
    }
}

static int64_t result_of(int64_t knoc)
{
    return knoc < 0 ? error_of(knoc) : knoc;
}

static uint64_t text_length(const char *text)
{
    uint64_t n = 0;

    while (text[n])
    {
        n++;
    }

    return n;
}

static void copy_text(char *to, const char *from, uint64_t room)
{
    uint64_t i = 0;

    while (from[i] && i + 1 < room)
    {
        to[i] = from[i];
        i++;
    }

    to[i] = 0;
}

static int text_equal(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }

    return *a == *b;
}

static void default_termios(uint8_t *t)
{
    uint32_t flags[4] = {0400 | 02000, 01 | 04, 017 | 060 | 0200, 01 | 02 | 010 | 020 | 040 | 01000 | 04000 | 0100000};

    memset(t, 0, 36);
    memcpy(t, flags, sizeof(flags));
    t[17 + 0] = 3;
    t[17 + 1] = 0x1c;
    t[17 + 2] = 0x7f;
    t[17 + 3] = 0x15;
    t[17 + 4] = 4;
    t[17 + 5] = 0;
    t[17 + 6] = 1;
}

static uint32_t lflag(linux_state_t *s)
{
    uint32_t flags;

    memcpy(&flags, s->termios + 12, 4);
    return flags;
}

static int push_string(uint8_t *stack, uintptr_t top, uint64_t *used, uint64_t size, const char *text, uintptr_t *where)
{
    uint64_t length = text_length(text) + 1;

    if (*used + length > size / 2)
    {
        return -1;
    }

    *used += length;
    memcpy(stack + size - *used, text, length);
    *where = top - *used;
    return 0;
}

void *linux_create(const char *exe, const char *name, const char *args, const char *cwd,
                   const elf_linux_info_t *info, uint8_t *stack, uintptr_t stack_top, uint64_t stack_size,
                   uintptr_t *sp)
{
    linux_state_t *s = kmalloc(sizeof(linux_state_t));
    char buffer[ARGS_MAX];
    uintptr_t argv[40];
    uintptr_t envp[16];
    int argc = 0;
    int envc = 0;
    uint64_t used = 0;

    if (s == 0)
    {
        return 0;
    }

    memset(s, 0, sizeof(*s));
    s->fds[0].kind = FD_CONSOLE_IN;
    s->fds[1].kind = FD_CONSOLE_OUT;
    s->fds[2].kind = FD_CONSOLE_OUT;
    s->brk_start = info->brk;
    s->brk_now = info->brk;
    s->brk_mapped = info->brk;
    s->mmap_next = LINUX_MMAP_BASE;
    default_termios(s->termios);
    copy_text(s->exe, exe, sizeof(s->exe));
    copy_text(s->name, name, sizeof(s->name));

    if (push_string(stack, stack_top, &used, stack_size, exe, &argv[argc++]) != 0)
    {
        kfree(s);
        return 0;
    }

    copy_text(buffer, args, sizeof(buffer));

    for (char *p = buffer; *p && argc < 39;)
    {
        while (*p == ' ')
        {
            p++;
        }

        if (!*p)
        {
            break;
        }

        char *start = p;
        char quote = 0;

        if (*p == '"' || *p == '\'')
        {
            quote = *p++;
            start = p;

            while (*p && *p != quote)
            {
                p++;
            }
        }
        else
        {
            while (*p && *p != ' ')
            {
                p++;
            }
        }

        char saved = *p;

        *p = 0;
        push_string(stack, stack_top, &used, stack_size, start, &argv[argc++]);
        *p = saved;

        if (*p)
        {
            p++;
        }
    }

    for (unsigned long i = 0; i < sizeof(environment) / sizeof(environment[0]); i++)
    {
        push_string(stack, stack_top, &used, stack_size, environment[i], &envp[envc++]);
    }

    char pwd[PATH_MAX + 4] = "PWD=";

    copy_text(pwd + 4, cwd, sizeof(pwd) - 4);
    push_string(stack, stack_top, &used, stack_size, pwd, &envp[envc++]);

    uintptr_t platform;
    uintptr_t random_bytes_address;
    uint8_t random[16];

    push_string(stack, stack_top, &used, stack_size, "riscv64", &platform);

    if (virtio_rng_read(random, sizeof(random)) != (int64_t)sizeof(random))
    {
        for (int i = 0; i < 16; i++)
        {
            random[i] = (uint8_t)(timer_read() >> (i % 8));
        }
    }

    used = (used + 16 + 15) & ~15UL;
    memcpy(stack + stack_size - used, random, 16);
    random_bytes_address = stack_top - used;

    uint64_t hwcap = (1UL << ('I' - 'A')) | (1UL << ('M' - 'A')) | (1UL << ('A' - 'A')) | (1UL << ('F' - 'A')) |
                     (1UL << ('D' - 'A')) | (1UL << ('C' - 'A'));
    uint64_t auxv[] = {3,  info->phdr, 4,  info->phent, 5,  info->phnum,        6,  PAGE_SIZE,
                       7,  info->interp_base, 8,  0,    9,  info->entry,        11, 0,
                       12, 0,          13, 0,           14, 0,                  16, hwcap,
                       17, 100,        23, 0,           25, random_bytes_address, 31, argv[0],
                       15, platform,   51, 4096,        0,  0};
    uint64_t words = 1 + (uint64_t)argc + 1 + (uint64_t)envc + 1 + sizeof(auxv) / 8;
    uint64_t bottom = (used + words * 8 + 15) & ~15UL;
    uint64_t *out = (uint64_t *)(stack + stack_size - bottom);

    if (bottom > stack_size / 2)
    {
        kfree(s);
        return 0;
    }

    *out++ = (uint64_t)argc;

    for (int i = 0; i < argc; i++)
    {
        *out++ = argv[i];
    }

    *out++ = 0;

    for (int i = 0; i < envc; i++)
    {
        *out++ = envp[i];
    }

    *out++ = 0;
    memcpy(out, auxv, sizeof(auxv));
    *sp = stack_top - bottom;
    return s;
}

void linux_destroy(void *pointer)
{
    linux_state_t *s = pointer;

    for (int i = 0; i < FDS_MAX; i++)
    {
        if (s->fds[i].path)
        {
            kfree(s->fds[i].path);
        }

        if (s->fds[i].data)
        {
            kfree(s->fds[i].data);
        }
    }

    kfree(s);
}

static linux_fd_t *fd_get(int64_t fd)
{
    linux_state_t *s = state();

    if (fd < 0 || fd >= FDS_MAX || s->fds[fd].kind == FD_FREE)
    {
        return 0;
    }

    return &s->fds[fd];
}

static int fd_new(int from)
{
    linux_state_t *s = state();

    for (int i = from < 0 ? 0 : from; i < FDS_MAX; i++)
    {
        if (s->fds[i].kind == FD_FREE)
        {
            memset(&s->fds[i], 0, sizeof(s->fds[i]));
            return i;
        }
    }

    return -1;
}

static void fd_release(int fd)
{
    linux_state_t *s = state();
    linux_fd_t *f = &s->fds[fd];
    int shared = 0;

    if (f->kind == FD_FILE)
    {
        for (int i = 0; i < FDS_MAX; i++)
        {
            shared |= i != fd && s->fds[i].kind == FD_FILE && s->fds[i].knoc == f->knoc;
        }

        if (!shared)
        {
            kfile_close((uint64_t)f->knoc);
        }
    }

    if (f->path)
    {
        kfree(f->path);
    }

    if (f->data)
    {
        kfree(f->data);
    }

    memset(f, 0, sizeof(*f));
}

static int64_t full_path(int64_t dirfd, uintptr_t address, char *out)
{
    char raw[PATH_MAX];
    char joined[PATH_MAX * 2];

    if (user_copy_string(raw, address, PATH_MAX) != 0)
    {
        return -L_EFAULT;
    }

    if (raw[0] == 0)
    {
        return -L_ENOENT;
    }

    if (raw[0] == '/' || (int)dirfd == AT_FDCWD)
    {
        return process_resolve_path(raw, out) == 0 ? 0 : -L_EINVAL;
    }

    linux_fd_t *dir = fd_get(dirfd);

    if (!dir || dir->kind != FD_DIR)
    {
        return dir ? -L_ENOTDIR : -L_EBADF;
    }

    uint64_t n = text_length(dir->path);

    memcpy(joined, dir->path, n);
    joined[n] = '/';
    copy_text(joined + n + 1, raw, sizeof(joined) - n - 1);
    return process_resolve_path(joined, out) == 0 ? 0 : -L_EINVAL;
}

static int is_console_path(const char *path)
{
    return text_equal(path, "/dev/tty") || text_equal(path, "/dev/console") || text_equal(path, "/dev/stdin") ||
           text_equal(path, "/dev/stdout") || text_equal(path, "/dev/stderr");
}

typedef struct text
{
    char *data;
    uint32_t used;
    uint32_t room;
} text_t;

static void put_text(text_t *t, const char *s)
{
    while (*s && t->used + 1 < t->room)
    {
        t->data[t->used++] = *s++;
    }

    t->data[t->used] = 0;
}

static void put_number(text_t *t, uint64_t value)
{
    char digits[24];
    int n = 0;

    do
    {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);

    while (n > 0 && t->used + 1 < t->room)
    {
        t->data[t->used++] = digits[--n];
    }

    t->data[t->used] = 0;
}

static int starts_with(const char *text, const char *prefix)
{
    while (*prefix && *text == *prefix)
    {
        text++;
        prefix++;
    }

    return *prefix == 0;
}

static int64_t parse_pid(const char *text, const char **rest)
{
    int64_t value = 0;
    int digits = 0;

    if (starts_with(text, "self"))
    {
        *rest = text + 4;
        return process_current_pid();
    }

    while (*text >= '0' && *text <= '9')
    {
        value = value * 10 + (*text++ - '0');
        digits++;
    }

    *rest = text;
    return digits ? value : -1;
}

static int find_process(int64_t pid, process_info_t *info)
{
    for (uint32_t i = 0; process_info(i, info) == 0; i++)
    {
        if (info->pid == pid && pid > 0)
        {
            return 0;
        }
    }

    return -1;
}

static char state_letter(uint32_t state)
{
    return state == 2 ? 'R' : state == 1 ? 'R' : state == 5 ? 'Z' : 'S';
}

static int proc_file(const char *path, text_t *t)
{
    const char *rest;

    if (text_equal(path, "/proc/meminfo"))
    {
        uint64_t total = page_total() * PAGE_SIZE / 1024;
        uint64_t free = page_free_count() * PAGE_SIZE / 1024;

        put_text(t, "MemTotal:       ");
        put_number(t, total);
        put_text(t, " kB\nMemFree:        ");
        put_number(t, free);
        put_text(t, " kB\nMemAvailable:   ");
        put_number(t, free);
        put_text(t, " kB\nBuffers:        0 kB\nCached:         0 kB\nSwapTotal:      0 kB\nSwapFree:       0 kB\n");
        return 0;
    }

    if (text_equal(path, "/proc/mounts") || text_equal(path, "/proc/self/mounts"))
    {
        put_text(t, "knocfs / knocfs rw 0 0\n");
        return 0;
    }

    if (text_equal(path, "/proc/cpuinfo"))
    {
        for (int i = 0; i < 8; i++)
        {
            put_text(t, "processor\t: ");
            put_number(t, (uint64_t)i);
            put_text(t, "\nhart\t\t: ");
            put_number(t, (uint64_t)i);
            put_text(t, "\nisa\t\t: rv64imafdc\nmmu\t\t: sv39\n\n");
        }

        return 0;
    }

    if (text_equal(path, "/proc/uptime"))
    {
        uint64_t ticks = timer_ticks();

        put_number(t, ticks / TIMER_TICK_HZ);
        put_text(t, ".");
        put_number(t, (ticks % TIMER_TICK_HZ) / 10);
        put_text(t, " 0.00\n");
        return 0;
    }

    if (text_equal(path, "/proc/version"))
    {
        put_text(t, "Linux version 6.1.0-knocos (KnocOS " KNOCOS_VERSION " Linux layer)\n");
        return 0;
    }

    if (text_equal(path, "/proc/stat"))
    {
        cpu_info_t cpu;
        uint64_t busy = 0;
        uint64_t idle = 0;

        for (uint32_t i = 0; process_cpu_info(i, &cpu) == 0; i++)
        {
            busy += cpu.busy_ticks;
            idle += cpu.idle_ticks;
        }

        put_text(t, "cpu  ");
        put_number(t, busy);
        put_text(t, " 0 0 ");
        put_number(t, idle);
        put_text(t, " 0 0 0 0 0 0\n");

        for (uint32_t i = 0; process_cpu_info(i, &cpu) == 0; i++)
        {
            if (!cpu.online || cpu.role == CPU_ROLE_AI_SPACE)
            {
                continue;
            }

            put_text(t, "cpu");
            put_number(t, i);
            put_text(t, " ");
            put_number(t, cpu.busy_ticks);
            put_text(t, " 0 0 ");
            put_number(t, cpu.idle_ticks);
            put_text(t, " 0 0 0 0 0 0\n");
        }

        put_text(t, "intr 0\nctxt 0\nbtime ");
        put_number(t, rtc_seconds() - timer_ticks() / TIMER_TICK_HZ);
        put_text(t, "\nprocesses 0\nprocs_running 1\nprocs_blocked 0\n");
        return 0;
    }

    if (text_equal(path, "/proc/loadavg"))
    {
        put_text(t, "0.00 0.00 0.00 1/1 1\n");
        return 0;
    }

    if (!starts_with(path, "/proc/"))
    {
        return -1;
    }

    int64_t pid = parse_pid(path + 6, &rest);
    process_info_t info;

    if (pid < 0 || find_process(pid, &info) != 0)
    {
        return -1;
    }

    char letter[2] = {state_letter(info.state), 0};

    if (text_equal(rest, "/stat"))
    {
        put_number(t, (uint64_t)info.pid);
        put_text(t, " (");
        put_text(t, info.name);
        put_text(t, ") ");
        put_text(t, letter);
        put_text(t, " 1 ");
        put_number(t, (uint64_t)info.pid);
        put_text(t, " ");
        put_number(t, (uint64_t)info.pid);
        put_text(t, " 0 -1 0 0 0 0 0 ");
        put_number(t, info.cpu_ticks);
        put_text(t, " 0 0 0 20 0 1 0 0 ");
        put_number(t, info.memory);
        put_text(t, " ");
        put_number(t, info.memory / PAGE_SIZE);
        put_text(t, " 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
        return 0;
    }

    if (text_equal(rest, "/cmdline") || text_equal(rest, "/comm"))
    {
        put_text(t, info.name);

        if (text_equal(rest, "/comm"))
        {
            put_text(t, "\n");
        }
        else if (t->used + 1 < t->room)
        {
            t->used++;
        }

        return 0;
    }

    if (text_equal(rest, "/status"))
    {
        put_text(t, "Name:\t");
        put_text(t, info.name);
        put_text(t, "\nState:\t");
        put_text(t, letter);
        put_text(t, "\nPid:\t");
        put_number(t, (uint64_t)info.pid);
        put_text(t, "\nPPid:\t1\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nVmRSS:\t");
        put_number(t, info.memory / 1024);
        put_text(t, " kB\n");
        return 0;
    }

    return -1;
}

static int proc_directory(const char *path)
{
    const char *rest;

    if (text_equal(path, "/proc"))
    {
        return 1;
    }

    if (!starts_with(path, "/proc/"))
    {
        return 0;
    }

    int64_t pid = parse_pid(path + 6, &rest);
    process_info_t info;

    return pid >= 0 && rest[0] == 0 && find_process(pid, &info) == 0;
}

static int proc_entry(const char *path, uint32_t index, char *name, uint8_t *type)
{
    static const char *files[] = {"meminfo", "mounts", "cpuinfo", "uptime", "version", "loadavg", "stat"};
    static const char *pid_files[] = {"stat", "status", "cmdline", "comm"};
    process_info_t info;

    if (text_equal(path, "/proc"))
    {
        if (index < sizeof(files) / sizeof(files[0]))
        {
            copy_text(name, files[index], KNOCFS_NAME_MAX);
            *type = 8;
            return 0;
        }

        uint32_t wanted = index - (uint32_t)(sizeof(files) / sizeof(files[0]));
        uint32_t seen = 0;
        uint32_t i = 0;

        while (1)
        {
            if (process_info(i++, &info) != 0)
            {
                return -1;
            }

            if (info.pid > 0 && seen++ == wanted)
            {
                break;
            }
        }

        text_t t = {name, 0, KNOCFS_NAME_MAX};

        put_number(&t, (uint64_t)info.pid);
        *type = 4;
        return 0;
    }

    if (index < sizeof(pid_files) / sizeof(pid_files[0]))
    {
        copy_text(name, pid_files[index], KNOCFS_NAME_MAX);
        *type = 8;
        return 0;
    }

    return -1;
}

static void fill_stat(linux_stat_t *st, uint32_t inode, const knocfs_stat_t *ks, const char *path)
{
    int in_bin = path && path[0] == '/' && path[1] == 'b' && path[2] == 'i' && path[3] == 'n' && path[4] == '/';

    memset(st, 0, sizeof(*st));
    st->dev = 1;
    st->ino = inode;
    st->mode = ks->type == KNOCFS_TYPE_DIR ? 0040755 : ks->type == KNOCFS_TYPE_LINK ? 0120777 : in_bin ? 0100755 : 0100644;
    st->nlink = ks->type == KNOCFS_TYPE_DIR ? 2 : 1;
    st->size = (int64_t)ks->size;
    st->blksize = 4096;
    st->blocks = (int64_t)((ks->size + 511) / 512);
    st->mtime = (int64_t)ks->modified;
    st->atime = (int64_t)ks->modified;
    st->ctime = (int64_t)(ks->created ? ks->created : ks->modified);
}

static void char_stat(linux_stat_t *st, uint64_t rdev, uint32_t mode)
{
    memset(st, 0, sizeof(*st));
    st->dev = 5;
    st->mode = 0020000 | mode;
    st->nlink = 1;
    st->rdev = rdev;
    st->blksize = 1024;
}

static int64_t stat_path_mode(const char *path, linux_stat_t *st, int follow);

static int64_t stat_path(const char *path, linux_stat_t *st)
{
    return stat_path_mode(path, st, 1);
}

static int64_t stat_path_mode(const char *path, linux_stat_t *st, int follow)
{
    uint32_t inode;
    knocfs_stat_t ks;

    if (text_equal(path, "/dev/null"))
    {
        char_stat(st, 0x103, 0666);
        return 0;
    }

    if (is_console_path(path))
    {
        char_stat(st, 0x8800, 0620);
        return 0;
    }

    if (starts_with(path, "/proc"))
    {
        char scratch[PROC_TEXT_MAX];
        text_t t = {scratch, 0, sizeof(scratch)};

        memset(st, 0, sizeof(*st));
        st->dev = 3;
        st->nlink = 1;
        st->blksize = 1024;

        if (proc_directory(path))
        {
            st->mode = 0040555;
            return 0;
        }

        if (proc_file(path, &t) == 0)
        {
            st->mode = 0100444;
            return 0;
        }

        return -L_ENOENT;
    }

    if (!syscall_allowed(SYS_STAT, CAP_FILES_READ))
    {
        return -L_EACCES;
    }

    int result = follow ? knocfs_lookup(path, &inode) : knocfs_lookup_link(path, &inode);

    if (result == 0)
    {
        result = knocfs_stat(inode, &ks);
    }

    if (result != 0)
    {
        return error_of(result);
    }

    fill_stat(st, inode, &ks, path);
    return 0;
}

static int64_t stat_fd(linux_fd_t *f, linux_stat_t *st)
{
    if (f->kind == FD_CONSOLE_IN || f->kind == FD_CONSOLE_OUT)
    {
        char_stat(st, 0x8800, 0620);
        return 0;
    }

    if (f->kind == FD_NULL)
    {
        char_stat(st, 0x103, 0666);
        return 0;
    }

    if (f->kind == FD_DIR)
    {
        return stat_path(f->path, st);
    }

    if (f->kind == FD_MEMORY)
    {
        memset(st, 0, sizeof(*st));
        st->dev = 3;
        st->mode = 0100444;
        st->nlink = 1;
        st->blksize = 1024;
        return 0;
    }

    open_file_t *file = process_file(f->knoc);
    knocfs_stat_t ks;

    if (!file || knocfs_stat(file->inode, &ks) != 0)
    {
        return -L_EBADF;
    }

    fill_stat(st, file->inode, &ks, 0);
    return 0;
}

static int64_t do_openat(int64_t dirfd, uintptr_t address, uint64_t flags)
{
    char path[PATH_MAX];
    int64_t r = full_path(dirfd, address, path);
    uint32_t inode;
    knocfs_stat_t ks;
    uint32_t access = (uint32_t)(flags & 3);

    if (r != 0)
    {
        return r;
    }

    int fd = fd_new(0);

    if (fd < 0)
    {
        return -L_EMFILE;
    }

    linux_fd_t *f = &state()->fds[fd];

    f->access = access;

    if (text_equal(path, "/dev/null") || text_equal(path, "/dev/zero"))
    {
        f->kind = FD_NULL;
        return fd;
    }

    if (is_console_path(path))
    {
        f->kind = access == 0 ? FD_CONSOLE_IN : FD_CONSOLE_OUT;
        return fd;
    }

    if (starts_with(path, "/proc"))
    {
        if (proc_directory(path))
        {
            f->path = kmalloc(PATH_MAX);

            if (!f->path)
            {
                return -L_ENOMEM;
            }

            copy_text(f->path, path, PATH_MAX);
            f->kind = FD_DIR;
            return fd;
        }

        f->data = kmalloc(PROC_TEXT_MAX);

        if (!f->data)
        {
            return -L_ENOMEM;
        }

        text_t t = {f->data, 0, PROC_TEXT_MAX};

        if (proc_file(path, &t) != 0)
        {
            kfree(f->data);
            f->data = 0;
            return -L_ENOENT;
        }

        f->kind = FD_MEMORY;
        f->data_size = t.used;
        return fd;
    }

    int exists = knocfs_lookup(path, &inode) == 0 && knocfs_stat(inode, &ks) == 0;

    if (exists && (flags & 0100) && (flags & 0200))
    {
        return -L_EEXIST;
    }

    if (exists && ks.type == KNOCFS_TYPE_DIR)
    {
        if (access != 0)
        {
            return -L_EISDIR;
        }

        if (!syscall_allowed(SYS_READDIR, CAP_FILES_READ))
        {
            return -L_EACCES;
        }

        f->path = kmalloc(PATH_MAX);

        if (!f->path)
        {
            return -L_ENOMEM;
        }

        copy_text(f->path, path, PATH_MAX);
        f->kind = FD_DIR;
        return fd;
    }

    if (flags & 0200000)
    {
        return exists ? -L_ENOTDIR : -L_ENOENT;
    }

    uint64_t kflags = access == 0 ? O_READ : access == 1 ? O_WRITE : (O_READ | O_WRITE);

    kflags |= (flags & 0100) ? O_CREATE : 0;
    kflags |= (flags & 01000) ? O_TRUNC : 0;

    int64_t knoc = kfile_open(path, kflags);

    if (knoc < 0)
    {
        return error_of(knoc);
    }

    f->kind = FD_FILE;
    f->knoc = (int)knoc;
    f->append = (flags & 02000) != 0;
    return fd;
}

static int64_t console_read_line(linux_state_t *s, uintptr_t buffer, uint64_t length)
{
    uint32_t local = lflag(s);

    if (!(local & 02))
    {
        char chunk[256];
        int64_t n = tty_read(chunk, length < sizeof(chunk) ? length : sizeof(chunk));

        for (int64_t i = 0; i < n; i++)
        {
            if (chunk[i] == '\r' && (((uint32_t *)s->termios)[0] & 0400))
            {
                chunk[i] = '\n';
            }
        }

        if (n > 0 && (local & 010))
        {
            for (int64_t i = 0; i < n; i++)
            {
                uart_putc(chunk[i]);
            }
        }

        return user_copy_out(buffer, chunk, (uint64_t)n) == 0 ? n : -L_EFAULT;
    }

    while (s->line_used >= s->line_length)
    {
        s->line_length = 0;
        s->line_used = 0;

        while (1)
        {
            char c;

            tty_read(&c, 1);

            if (c == '\r' || c == '\n')
            {
                s->line[s->line_length++] = '\n';

                if (local & 010)
                {
                    uart_putc('\n');
                }

                break;
            }

            if (c == 4)
            {
                if (s->line_length == 0)
                {
                    return 0;
                }

                break;
            }

            if (c == 0x7f || c == 8)
            {
                if (s->line_length > 0)
                {
                    s->line_length--;

                    if (local & 010)
                    {
                        uart_puts("\b \b");
                    }
                }

                continue;
            }

            if (s->line_length < LINE_MAX - 1)
            {
                s->line[s->line_length++] = c;

                if (local & 010)
                {
                    uart_putc(c);
                }
            }
        }
    }

    uint64_t n = s->line_length - s->line_used;

    if (n > length)
    {
        n = length;
    }

    if (user_copy_out(buffer, s->line + s->line_used, n) != 0)
    {
        return -L_EFAULT;
    }

    s->line_used += (uint32_t)n;
    return (int64_t)n;
}

static int64_t do_read(int64_t fd, uintptr_t buffer, uint64_t length)
{
    linux_fd_t *f = fd_get(fd);

    if (!f)
    {
        return -L_EBADF;
    }

    switch (f->kind)
    {
    case FD_CONSOLE_IN:
        if (!syscall_allowed(SYS_READ, CAP_CONSOLE))
        {
            return -L_EACCES;
        }

        return console_read_line(state(), buffer, length);
    case FD_NULL:
        return 0;
    case FD_DIR:
        return -L_EISDIR;
    case FD_MEMORY:
    {
        uint64_t n = f->data_size - f->data_offset;

        if (n > length)
        {
            n = length;
        }

        if (user_copy_out(buffer, f->data + f->data_offset, n) != 0)
        {
            return -L_EFAULT;
        }

        f->data_offset += (uint32_t)n;
        return (int64_t)n;
    }
    case FD_FILE:
        return result_of(kfile_read((uint64_t)f->knoc, buffer, length));
    default:
        return -L_EBADF;
    }
}

static int64_t do_write(int64_t fd, uintptr_t buffer, uint64_t length)
{
    linux_fd_t *f = fd_get(fd);

    if (!f)
    {
        return -L_EBADF;
    }

    switch (f->kind)
    {
    case FD_CONSOLE_OUT:
    case FD_CONSOLE_IN:
        return result_of(kfile_write(FD_STDOUT, buffer, length));
    case FD_NULL:
        return (int64_t)length;
    case FD_DIR:
        return -L_EISDIR;
    case FD_FILE:
        if (f->append)
        {
            open_file_t *file = process_file(f->knoc);
            knocfs_stat_t ks;

            if (file && knocfs_stat(file->inode, &ks) == 0)
            {
                file->offset = ks.size;
            }
        }

        return result_of(kfile_write((uint64_t)f->knoc, buffer, length));
    default:
        return -L_EBADF;
    }
}

static int64_t do_vector(int64_t fd, uintptr_t vector, uint64_t count, int writing)
{
    int64_t total = 0;

    if (count > 1024)
    {
        return -L_EINVAL;
    }

    for (uint64_t i = 0; i < count; i++)
    {
        uint64_t iov[2];

        if (user_copy_in(iov, vector + i * 16, sizeof(iov)) != 0)
        {
            return total ? total : -L_EFAULT;
        }

        if (iov[1] == 0)
        {
            continue;
        }

        int64_t n = writing ? do_write(fd, iov[0], iov[1]) : do_read(fd, iov[0], iov[1]);

        if (n < 0)
        {
            return total ? total : n;
        }

        total += n;

        if ((uint64_t)n < iov[1])
        {
            break;
        }
    }

    return total;
}

static int64_t do_lseek(int64_t fd, int64_t offset, int64_t whence)
{
    linux_fd_t *f = fd_get(fd);

    if (!f)
    {
        return -L_EBADF;
    }

    if (f->kind == FD_DIR)
    {
        if (offset == 0 && whence == 0)
        {
            f->dir_index = 0;
            return 0;
        }

        return -L_EINVAL;
    }

    if (f->kind == FD_MEMORY)
    {
        int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->data_offset : (int64_t)f->data_size;

        if (base + offset < 0 || base + offset > (int64_t)f->data_size)
        {
            return -L_EINVAL;
        }

        f->data_offset = (uint32_t)(base + offset);
        return base + offset;
    }

    if (f->kind != FD_FILE)
    {
        return -L_ESPIPE;
    }

    open_file_t *file = process_file(f->knoc);
    knocfs_stat_t ks;

    if (!file || knocfs_stat(file->inode, &ks) != 0)
    {
        return -L_EBADF;
    }

    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)file->offset : whence == 2 ? (int64_t)ks.size : -1;

    if (base < 0 || base + offset < 0)
    {
        return -L_EINVAL;
    }

    file->offset = (uint64_t)(base + offset);
    return (int64_t)file->offset;
}

static int64_t do_pio(int64_t fd, uintptr_t buffer, uint64_t length, int64_t offset, int writing)
{
    linux_fd_t *f = fd_get(fd);

    if (!f || f->kind != FD_FILE)
    {
        return f ? -L_ESPIPE : -L_EBADF;
    }

    open_file_t *file = process_file(f->knoc);

    if (!file)
    {
        return -L_EBADF;
    }

    uint64_t saved = file->offset;

    file->offset = (uint64_t)offset;

    int64_t n = writing ? kfile_write((uint64_t)f->knoc, buffer, length) : kfile_read((uint64_t)f->knoc, buffer, length);

    file->offset = saved;
    return result_of(n);
}

static int64_t do_getdents(int64_t fd, uintptr_t buffer, uint64_t count)
{
    linux_fd_t *f = fd_get(fd);
    uint32_t directory;
    uint8_t *out;
    uint64_t used = 0;

    if (!f || f->kind != FD_DIR)
    {
        return f ? -L_ENOTDIR : -L_EBADF;
    }

    int proc = starts_with(f->path, "/proc");

    if (!proc && knocfs_lookup(f->path, &directory) != 0)
    {
        return -L_ENOENT;
    }

    if (proc)
    {
        directory = 1;
    }

    if (count > DIRENT_BUFFER)
    {
        count = DIRENT_BUFFER;
    }

    out = kmalloc(DIRENT_BUFFER);

    if (!out)
    {
        return -L_ENOMEM;
    }

    while (1)
    {
        knocfs_dirent_t entry;
        knocfs_stat_t ks;
        const char *name;
        uint32_t inode;
        uint8_t type;

        if (f->dir_index == 0 || f->dir_index == 1)
        {
            name = f->dir_index == 0 ? "." : "..";
            inode = directory;
            type = 4;
        }
        else if (proc)
        {
            if (proc_entry(f->path, f->dir_index - 2, entry.name, &type) != 0)
            {
                break;
            }

            name = entry.name;
            inode = f->dir_index + 1000;
        }
        else
        {
            if (knocfs_readdir(directory, f->dir_index - 2, &entry) != 0)
            {
                break;
            }

            name = entry.name;
            inode = entry.inode;
            type = knocfs_stat(entry.inode, &ks) != 0 ? 8
                   : ks.type == KNOCFS_TYPE_DIR ? 4
                   : ks.type == KNOCFS_TYPE_LINK ? 10
                                                  : 8;
        }

        uint64_t name_length = text_length(name);
        uint64_t record = (19 + name_length + 1 + 7) & ~7UL;

        if (used + record > count)
        {
            if (used == 0)
            {
                kfree(out);
                return -L_EINVAL;
            }

            break;
        }

        uint64_t ino = inode;
        int64_t next = (int64_t)f->dir_index + 1;
        uint16_t length16 = (uint16_t)record;

        memset(out + used, 0, record);
        memcpy(out + used, &ino, 8);
        memcpy(out + used + 8, &next, 8);
        memcpy(out + used + 16, &length16, 2);
        out[used + 18] = type;
        memcpy(out + used + 19, name, name_length);
        used += record;
        f->dir_index++;
    }

    int64_t result = user_copy_out(buffer, out, used) == 0 ? (int64_t)used : -L_EFAULT;

    kfree(out);
    return result;
}

static uint64_t rwx_of(uint64_t prot)
{
    return ((prot & 1) ? PTE_R : 0) | ((prot & 2) ? PTE_W : 0) | ((prot & 4) ? PTE_X : 0);
}

static void release_range(uintptr_t address, uint64_t length)
{
    for (uintptr_t page = address; page < address + length; page += PAGE_SIZE)
    {
        process_page_free(page);
    }
}

static int64_t do_brk(uintptr_t address)
{
    linux_state_t *s = state();

    if (address == 0 || address < s->brk_start || address >= LINUX_INTERP_BASE)
    {
        return (int64_t)s->brk_now;
    }

    if (address > s->brk_mapped)
    {
        uintptr_t end = (address + BRK_STEP - 1) & ~(BRK_STEP - 1);

        for (uintptr_t page = s->brk_mapped; page < end; page += PAGE_SIZE)
        {
            if (!process_page_new(page, PTE_R | PTE_W))
            {
                release_range(s->brk_mapped, page - s->brk_mapped);
                return (int64_t)s->brk_now;
            }
        }

        s->brk_mapped = end;
    }

    s->brk_now = address;
    asm volatile("sfence.vma zero, zero");
    return (int64_t)address;
}

static int64_t do_mmap(uintptr_t address, uint64_t length, uint64_t prot, uint64_t flags, int64_t fd, uint64_t offset)
{
    linux_state_t *s = state();
    int anonymous = (flags & 0x20) != 0;
    open_file_t *file = 0;

    if (length == 0 || (offset & (PAGE_SIZE - 1)) || ((flags & 0x10) && (address & (PAGE_SIZE - 1))))
    {
        return -L_EINVAL;
    }

    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (!anonymous)
    {
        linux_fd_t *f = fd_get(fd);

        if (!f || (f->kind != FD_FILE && f->kind != FD_NULL))
        {
            return -L_EBADF;
        }

        file = f->kind == FD_FILE ? process_file(f->knoc) : 0;
        anonymous = file == 0;
    }

    uintptr_t where;

    if (flags & 0x10)
    {
        where = address;
        release_range(where, length);
    }
    else
    {
        where = s->mmap_next;
        s->mmap_next += length + PAGE_SIZE;
    }

    uint64_t rwx = rwx_of(prot);

    for (uint64_t done = 0; done < length; done += PAGE_SIZE)
    {
        if (rwx == 0 && anonymous)
        {
            if (process_page_reserve(where + done) != 0)
            {
                release_range(where, done);
                return -L_ENOMEM;
            }

            continue;
        }

        uint8_t *memory = process_page_new(where + done, rwx ? rwx : PTE_R);

        if (!memory)
        {
            release_range(where, done);
            return -L_ENOMEM;
        }

        if (file)
        {
            knocfs_read(file->inode, offset + done, memory, PAGE_SIZE);
        }

        if (rwx == 0)
        {
            process_page_protect(where + done, 0);
        }
    }

    asm volatile("sfence.vma zero, zero");
    return (int64_t)where;
}

static int64_t do_munmap(uintptr_t address, uint64_t length)
{
    if (address & (PAGE_SIZE - 1))
    {
        return -L_EINVAL;
    }

    release_range(address, (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    asm volatile("sfence.vma zero, zero");
    return 0;
}

static int64_t do_mprotect(uintptr_t address, uint64_t length, uint64_t prot)
{
    if (address & (PAGE_SIZE - 1))
    {
        return -L_EINVAL;
    }

    uint64_t rwx = rwx_of(prot);
    uintptr_t end = address + ((length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));

    for (uintptr_t page = address; page < end; page += PAGE_SIZE)
    {
        if (!process_page_exists(page))
        {
            return -L_ENOMEM;
        }
    }

    for (uintptr_t page = address; page < end; page += PAGE_SIZE)
    {
        if (process_page_protect(page, rwx) != 0)
        {
            return -L_ENOMEM;
        }
    }

    asm volatile("sfence.vma zero, zero");
    return 0;
}

static int64_t put_time(uintptr_t address, uint64_t seconds, uint64_t nanoseconds)
{
    uint64_t value[2] = {seconds, nanoseconds};

    return user_copy_out(address, value, sizeof(value)) == 0 ? 0 : -L_EFAULT;
}

static int64_t do_clock_gettime(uint64_t clock, uintptr_t address)
{
    uint64_t now = timer_read();

    if (clock == 0 || clock == 5 || clock == 8 || clock == 11)
    {
        uint64_t seconds = rtc_seconds();

        return put_time(address, seconds, (now % TIMER_FREQ_HZ) * (1000000000UL / TIMER_FREQ_HZ));
    }

    return put_time(address, now / TIMER_FREQ_HZ, (now % TIMER_FREQ_HZ) * (1000000000UL / TIMER_FREQ_HZ));
}

static int64_t do_sleep(uintptr_t request, int absolute, uint64_t clock)
{
    uint64_t t[2];

    if (user_copy_in(t, request, sizeof(t)) != 0)
    {
        return -L_EFAULT;
    }

    uint64_t ticks = t[0] * TIMER_TICK_HZ + (t[1] + 9999999) / 10000000;

    if (absolute)
    {
        uint64_t now = clock == 0 ? rtc_seconds() : timer_read() / TIMER_FREQ_HZ;

        ticks = t[0] > now ? (t[0] - now) * TIMER_TICK_HZ : 0;
    }

    if (ticks > 0)
    {
        process_sleep(ticks);
    }

    return 0;
}

static int64_t do_uname(uintptr_t address)
{
    char buffer[6 * 65];
    const char *fields[6] = {"Linux", "knocos", "6.1.0-knocos", KNOCOS_VERSION " (KnocOS Linux layer)", "riscv64",
                             "(none)"};

    memset(buffer, 0, sizeof(buffer));

    for (int i = 0; i < 6; i++)
    {
        copy_text(buffer + i * 65, fields[i], 65);
    }

    return user_copy_out(address, buffer, sizeof(buffer)) == 0 ? 0 : -L_EFAULT;
}

static int64_t do_ioctl(int64_t fd, uint64_t request, uintptr_t argument)
{
    linux_fd_t *f = fd_get(fd);
    linux_state_t *s = state();

    if (!f)
    {
        return -L_EBADF;
    }

    if (f->kind != FD_CONSOLE_IN && f->kind != FD_CONSOLE_OUT)
    {
        return -L_ENOTTY;
    }

    switch (request)
    {
    case 0x5401:
        return user_copy_out(argument, s->termios, sizeof(s->termios)) == 0 ? 0 : -L_EFAULT;
    case 0x5402:
    case 0x5403:
    case 0x5404:
        return user_copy_in(s->termios, argument, sizeof(s->termios)) == 0 ? 0 : -L_EFAULT;
    case 0x5413:
    {
        uint16_t size[4] = {24, 80, 0, 0};

        return user_copy_out(argument, size, sizeof(size)) == 0 ? 0 : -L_EFAULT;
    }
    case 0x5414:
        return 0;
    case 0x540F:
    {
        int32_t group = process_current_pid();

        return user_copy_out(argument, &group, sizeof(group)) == 0 ? 0 : -L_EFAULT;
    }
    case 0x5410:
        return 0;
    case 0x541B:
    {
        int32_t zero = 0;

        return user_copy_out(argument, &zero, sizeof(zero)) == 0 ? 0 : -L_EFAULT;
    }
    default:
        return -L_ENOTTY;
    }
}

static int64_t do_fcntl(int64_t fd, uint64_t command, uint64_t argument)
{
    linux_fd_t *f = fd_get(fd);

    if (!f)
    {
        return -L_EBADF;
    }

    switch (command)
    {
    case 0:
    case 1030:
    {
        int copy = fd_new((int)argument);

        if (copy < 0)
        {
            return -L_EMFILE;
        }

        linux_state_t *s = state();

        s->fds[copy] = *f;

        if (f->path)
        {
            s->fds[copy].path = kmalloc(PATH_MAX);

            if (s->fds[copy].path)
            {
                copy_text(s->fds[copy].path, f->path, PATH_MAX);
            }
        }

        return copy;
    }
    case 1:
    case 2:
    case 4:
        return 0;
    case 3:
        return f->access | (f->append ? 02000 : 0);
    default:
        return -L_EINVAL;
    }
}

static int64_t do_dup3(int64_t old, int64_t target)
{
    linux_fd_t *f = fd_get(old);
    linux_state_t *s = state();

    if (!f || target < 0 || target >= FDS_MAX)
    {
        return -L_EBADF;
    }

    if (old == target)
    {
        return target;
    }

    if (s->fds[target].kind != FD_FREE)
    {
        fd_release((int)target);
    }

    s->fds[target] = *f;

    if (f->path)
    {
        s->fds[target].path = kmalloc(PATH_MAX);

        if (s->fds[target].path)
        {
            copy_text(s->fds[target].path, f->path, PATH_MAX);
        }
    }

    return target;
}

static int64_t do_prlimit(uint64_t resource, uintptr_t old)
{
    uint64_t limit[2] = {~0UL, ~0UL};

    if (resource == 3)
    {
        limit[0] = 8UL * 1024 * 1024;
    }
    else if (resource == 7)
    {
        limit[0] = FDS_MAX;
        limit[1] = FDS_MAX;
    }

    if (old && user_copy_out(old, limit, sizeof(limit)) != 0)
    {
        return -L_EFAULT;
    }

    return 0;
}

static int64_t do_getrandom(uintptr_t buffer, uint64_t length)
{
    uint8_t chunk[RANDOM_MAX];
    uint64_t done = 0;

    while (done < length)
    {
        uint64_t n = length - done < RANDOM_MAX ? length - done : RANDOM_MAX;

        if (virtio_rng_read(chunk, n) != (int64_t)n)
        {
            return done ? (int64_t)done : -L_ENOSYS;
        }

        if (user_copy_out(buffer + done, chunk, n) != 0)
        {
            return -L_EFAULT;
        }

        done += n;
    }

    return (int64_t)done;
}

static int64_t timeout_ticks(uintptr_t address, int64_t *ticks)
{
    uint64_t t[2];

    if (address == 0)
    {
        *ticks = -1;
        return 0;
    }

    if (user_copy_in(t, address, sizeof(t)) != 0)
    {
        return -L_EFAULT;
    }

    *ticks = (int64_t)(t[0] * TIMER_TICK_HZ + (t[1] + 9999999) / 10000000);
    return 0;
}

static int console_ready(int64_t fd)
{
    linux_fd_t *f = fd_get(fd);

    return f && f->kind == FD_CONSOLE_IN ? tty_has_input() : 1;
}

static int64_t poll_once(uintptr_t fds, uint64_t count)
{
    int64_t ready = 0;

    for (uint64_t i = 0; i < count && i < 64; i++)
    {
        uint8_t entry[8];
        int32_t fd;
        int16_t events;
        int16_t revents = 0;

        if (user_copy_in(entry, fds + i * 8, 8) != 0)
        {
            return -L_EFAULT;
        }

        memcpy(&fd, entry, 4);
        memcpy(&events, entry + 4, 2);

        if (fd >= 0)
        {
            if (!fd_get(fd))
            {
                revents = 0x20;
            }
            else
            {
                revents = (int16_t)(events & 0x4);

                if ((events & 0x1) && console_ready(fd))
                {
                    revents |= 0x1;
                }
            }
        }

        memcpy(entry + 6, &revents, 2);

        if (user_copy_out(fds + i * 8, entry, 8) != 0)
        {
            return -L_EFAULT;
        }

        ready += revents != 0;
    }

    return ready;
}

static int64_t do_poll(uintptr_t fds, uint64_t count, uintptr_t timeout)
{
    int64_t ticks;
    int64_t r = timeout_ticks(timeout, &ticks);

    if (r != 0)
    {
        return r;
    }

    uint64_t deadline = timer_ticks() + (uint64_t)(ticks < 0 ? 0 : ticks);

    while (1)
    {
        int64_t ready = poll_once(fds, count);

        if (ready != 0 || ticks == 0 || (ticks > 0 && timer_ticks() >= deadline))
        {
            return ready;
        }

        tty_wait_input(ticks < 0 ? TIMER_TICK_HZ : deadline - timer_ticks());
    }
}

static int64_t select_once(uint64_t count, uintptr_t readers, uint8_t *in, uint8_t *out, uint64_t bytes)
{
    int64_t ready = 0;

    memcpy(out, in, bytes);

    for (uint64_t fd = 0; fd < count; fd++)
    {
        if ((in[fd / 8] >> (fd % 8)) & 1)
        {
            if (console_ready((int64_t)fd))
            {
                ready++;
            }
            else
            {
                out[fd / 8] &= (uint8_t)~(1 << (fd % 8));
            }
        }
    }

    if (readers && user_copy_out(readers, out, bytes) != 0)
    {
        return -L_EFAULT;
    }

    return ready;
}

static int64_t do_select(uint64_t count, uintptr_t readers, uintptr_t writers, uintptr_t timeout)
{
    uint8_t in[128];
    uint8_t out[128];
    uint8_t writing[128];
    uint64_t bytes = (count + 7) / 8;
    int64_t ticks;
    int64_t extra = 0;

    if (count > 1024)
    {
        return -L_EINVAL;
    }

    memset(in, 0, sizeof(in));

    if ((readers && user_copy_in(in, readers, bytes) != 0) || timeout_ticks(timeout, &ticks) != 0)
    {
        return -L_EFAULT;
    }

    if (writers && user_copy_in(writing, writers, bytes) == 0)
    {
        for (uint64_t fd = 0; fd < count; fd++)
        {
            extra += (writing[fd / 8] >> (fd % 8)) & 1;
        }
    }

    uint64_t deadline = timer_ticks() + (uint64_t)(ticks < 0 ? 0 : ticks);

    while (1)
    {
        int64_t ready = select_once(count, readers, in, out, bytes);

        if (ready < 0)
        {
            return ready;
        }

        if (ready + extra != 0 || ticks == 0 || (ticks > 0 && timer_ticks() >= deadline))
        {
            return ready + extra;
        }

        tty_wait_input(ticks < 0 ? TIMER_TICK_HZ : deadline - timer_ticks());
    }
}

static int64_t do_readlink(int64_t dirfd, uintptr_t path_address, uintptr_t buffer, uint64_t size)
{
    char path[PATH_MAX];
    int64_t r = full_path(dirfd, path_address, path);
    linux_state_t *s = state();

    if (r != 0)
    {
        return r;
    }

    if (text_equal(path, "/proc/self/exe"))
    {
        uint64_t n = text_length(s->exe);

        if (n > size)
        {
            n = size;
        }

        return user_copy_out(buffer, s->exe, n) == 0 ? (int64_t)n : -L_EFAULT;
    }

    char target[PATH_MAX];
    int64_t n = knocfs_readlink(path, target, sizeof(target));

    if (n < 0)
    {
        return error_of(n);
    }

    if ((uint64_t)n > size)
    {
        n = (int64_t)size;
    }

    return user_copy_out(buffer, target, (uint64_t)n) == 0 ? n : -L_EFAULT;
}

static int64_t do_sysinfo(uintptr_t address)
{
    uint8_t info[112];
    uint64_t uptime = timer_ticks() / TIMER_TICK_HZ;
    uint64_t total = page_total() * PAGE_SIZE;
    uint64_t free = page_free_count() * PAGE_SIZE;
    uint16_t procs = 8;
    uint32_t unit = 1;

    memset(info, 0, sizeof(info));
    memcpy(info, &uptime, 8);
    memcpy(info + 32, &total, 8);
    memcpy(info + 40, &free, 8);
    memcpy(info + 80, &procs, 2);
    memcpy(info + 104, &unit, 4);
    return user_copy_out(address, info, sizeof(info)) == 0 ? 0 : -L_EFAULT;
}

static int64_t do_statfs(uintptr_t address)
{
    uint64_t total = 0;
    uint64_t free = 0;
    uint64_t info[15];

    knocfs_space(&total, &free);
    memset(info, 0, sizeof(info));
    info[0] = 0x4B4E4F43;
    info[1] = 4096;
    info[2] = total / 4096;
    info[3] = free / 4096;
    info[4] = free / 4096;
    info[5] = 1024;
    info[6] = 512;
    info[8] = FILE_NAME_MAX - 1;
    info[9] = 4096;
    return user_copy_out(address, info, sizeof(info)) == 0 ? 0 : -L_EFAULT;
}

static void warn_unsupported(uint64_t number)
{
    linux_state_t *s = state();

    for (int i = 0; i < s->warned_count; i++)
    {
        if (s->warned[i] == number)
        {
            return;
        }
    }

    if (s->warned_count < WARNED_MAX)
    {
        s->warned[s->warned_count++] = (uint16_t)number;
    }

    uart_puts("[LINUX] ");
    uart_puts(s->name);
    uart_puts(": Linux system call ");
    uart_put_uint(number);
    uart_puts(" is not supported yet\n");
}

static uint64_t trace_id(uint64_t n)
{
    static const uint16_t map[][2] = {
        {63, SYS_READ},    {64, SYS_WRITE},   {65, SYS_READ},      {66, SYS_WRITE},   {67, SYS_READ},
        {68, SYS_WRITE},   {56, SYS_OPEN},    {57, SYS_CLOSE},     {62, SYS_SEEK},    {79, SYS_STAT},
        {80, SYS_STAT},    {61, SYS_READDIR}, {34, SYS_MKDIR},     {35, SYS_REMOVE},  {214, SYS_MEM_ALLOC},
        {222, SYS_MEM_ALLOC}, {93, SYS_EXIT}, {94, SYS_EXIT},      {172, SYS_GETPID}, {178, SYS_GETPID},
        {124, SYS_YIELD},  {101, SYS_SLEEP},  {115, SYS_SLEEP},    {113, SYS_UPTIME}, {169, SYS_UPTIME},
        {38, SYS_RENAME},  {276, SYS_RENAME}, {49, SYS_CHDIR},     {17, SYS_GETCWD},  {278, SYS_GETRANDOM},
        {160, SYS_SYSINFO}, {179, SYS_SYSINFO},
    };

    for (unsigned long i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    {
        if (map[i][0] == n)
        {
            return map[i][1];
        }
    }

    return 255;
}

int64_t linux_syscall(trap_frame_t *frame)
{
    uint64_t n = frame->a7;
    uint64_t a0 = frame->a0;
    uint64_t a1 = frame->a1;
    uint64_t a2 = frame->a2;
    uint64_t a3 = frame->a3;
    char path[PATH_MAX];
    char other[PATH_MAX];
    linux_stat_t st;
    int64_t r;

    process_record_syscall(trace_id(n));

    switch (n)
    {
    case 17:
    {
        const char *cwd = process_cwd();
        uint64_t length = text_length(cwd) + 1;

        if (length > a1)
        {
            return -L_ERANGE;
        }

        return user_copy_out(a0, cwd, length) == 0 ? (int64_t)length : -L_EFAULT;
    }
    case 43:
    case 44:
        return do_statfs(a1);
    case 23:
        return do_fcntl((int64_t)a0, 0, 0);
    case 24:
        return do_dup3((int64_t)a0, (int64_t)a1);
    case 25:
        return do_fcntl((int64_t)a0, a1, a2);
    case 29:
        return do_ioctl((int64_t)a0, a1, a2);
    case 34:
        r = full_path((int64_t)a0, a1, path);
        return r != 0 ? r : error_of(kfile_path_change(SYS_MKDIR, path));
    case 35:
        r = full_path((int64_t)a0, a1, path);
        return r != 0 ? r : error_of(kfile_path_change(SYS_REMOVE, path));
    case 36:
    {
        char target[PATH_MAX];

        if (user_copy_string(target, a0, sizeof(target)) != 0)
        {
            return -L_EFAULT;
        }

        r = full_path((int64_t)a1, a2, path);

        if (r != 0)
        {
            return r;
        }

        if (!syscall_allowed(SYS_OPEN, CAP_FILES_WRITE))
        {
            return -L_EACCES;
        }

        return error_of(knocfs_symlink(target, path));
    }
    case 37:
        return -L_EPERM;
    case 38:
    case 276:
        r = full_path((int64_t)a0, a1, path);

        if (r == 0)
        {
            r = full_path((int64_t)a2, a3, other);
        }

        return r != 0 ? r : error_of(kfile_rename(path, other));
    case 46:
    {
        linux_fd_t *f = fd_get((int64_t)a0);
        open_file_t *file = f && f->kind == FD_FILE ? process_file(f->knoc) : 0;

        if (!file)
        {
            return -L_EBADF;
        }

        return a1 == 0 ? error_of(knocfs_truncate(file->inode)) : 0;
    }
    case 48:
    case 439:
        r = full_path((int64_t)a0, a1, path);
        return r != 0 ? r : stat_path(path, &st);
    case 49:
    {
        uint32_t inode;
        knocfs_stat_t ks;

        r = full_path(AT_FDCWD, a0, path);

        if (r != 0)
        {
            return r;
        }

        if (proc_directory(path))
        {
            return process_chdir(path);
        }

        if (knocfs_lookup(path, &inode) != 0 || knocfs_stat(inode, &ks) != 0)
        {
            return -L_ENOENT;
        }

        return ks.type == KNOCFS_TYPE_DIR ? process_chdir(path) : -L_ENOTDIR;
    }
    case 50:
    {
        linux_fd_t *f = fd_get((int64_t)a0);

        return f && f->kind == FD_DIR ? process_chdir(f->path) : -L_ENOTDIR;
    }
    case 52:
    case 53:
    case 54:
    case 55:
    case 81:
    case 82:
    case 83:
    case 88:
        return 0;
    case 56:
        return do_openat((int64_t)a0, a1, a2);
    case 57:
        if (!fd_get((int64_t)a0))
        {
            return -L_EBADF;
        }

        fd_release((int)a0);
        return 0;
    case 61:
        return do_getdents((int64_t)a0, a1, a2);
    case 62:
        return do_lseek((int64_t)a0, (int64_t)a1, (int64_t)a2);
    case 63:
        return do_read((int64_t)a0, a1, a2);
    case 64:
        return do_write((int64_t)a0, a1, a2);
    case 65:
        return do_vector((int64_t)a0, a1, a2, 0);
    case 66:
        return do_vector((int64_t)a0, a1, a2, 1);
    case 67:
        return do_pio((int64_t)a0, a1, a2, (int64_t)a3, 0);
    case 68:
        return do_pio((int64_t)a0, a1, a2, (int64_t)a3, 1);
    case 71:
        return -L_EINVAL;
    case 72:
        return do_select(a0, a1, a2, frame->a4);
    case 73:
        return do_poll(a0, a1, a2);
    case 78:
        return do_readlink((int64_t)a0, a1, a2, a3);
    case 79:
    {
        char raw[2];

        if ((a3 & AT_EMPTY_PATH) && user_copy_in(raw, a1, 1) == 0 && raw[0] == 0)
        {
            linux_fd_t *f = fd_get((int64_t)a0);

            r = f ? stat_fd(f, &st) : -L_EBADF;
        }
        else
        {
            r = full_path((int64_t)a0, a1, path);
            r = r != 0 ? r : stat_path_mode(path, &st, !(a3 & 0x100));
        }

        return r != 0 ? r : user_copy_out(a2, &st, sizeof(st)) == 0 ? 0 : -L_EFAULT;
    }
    case 80:
    {
        linux_fd_t *f = fd_get((int64_t)a0);

        r = f ? stat_fd(f, &st) : -L_EBADF;
        return r != 0 ? r : user_copy_out(a1, &st, sizeof(st)) == 0 ? 0 : -L_EFAULT;
    }
    case 93:
    case 94:
        process_exit_code((int)(a0 & 0xFF));
    case 96:
    case 178:
    case 172:
        return process_current_pid();
    case 98:
    case 99:
    case 167:
    case 132:
    case 259:
    case 154:
    case 164:
    case 227:
    case 233:
        return 0;
    case 101:
        return do_sleep(a0, 0, 1);
    case 113:
        return do_clock_gettime(a0, a1);
    case 114:
        return a1 ? put_time(a1, 0, 1000000000UL / TIMER_TICK_HZ) : 0;
    case 115:
        return do_sleep(a2, (a1 & 1) != 0, a0);
    case 123:
    {
        uint64_t mask = 1;

        return user_copy_out(a2, &mask, 8) == 0 ? 8 : -L_EFAULT;
    }
    case 124:
        process_yield();
        return 0;
    case 129:
    case 130:
    case 131:
    {
        uint64_t target = n == 131 ? a1 : a0;
        uint64_t signal = n == 131 ? a2 : a1;

        if (signal != 0 && (target == 0 || target == (uint64_t)process_current_pid()))
        {
            process_exit_code(128 + (int)signal);
        }

        return 0;
    }
    case 134:
        if (a2)
        {
            uint8_t zero[32];

            memset(zero, 0, sizeof(zero));
            user_copy_out(a2, zero, sizeof(zero));
        }

        return 0;
    case 135:
        if (a2)
        {
            uint64_t zero = 0;

            user_copy_out(a2, &zero, 8);
        }

        return 0;
    case 155:
    case 156:
    case 157:
        return process_current_pid();
    case 148:
    case 150:
    {
        uint32_t zero = 0;

        user_copy_out(a0, &zero, 4);
        user_copy_out(a1, &zero, 4);
        user_copy_out(a2, &zero, 4);
        return 0;
    }
    case 198:
    case 199:
        return -97;
    case 200:
    case 201:
    case 202:
    case 203:
    case 204:
    case 205:
    case 206:
    case 207:
    case 208:
    case 209:
    case 210:
    case 211:
    case 212:
        return -88;
    case 158:
    case 174:
    case 175:
    case 176:
    case 177:
        return 0;
    case 160:
        return do_uname(a0);
    case 163:
    case 261:
        return do_prlimit(n == 163 ? a0 : a1, n == 163 ? a1 : a3);
    case 165:
    {
        uint8_t usage[144];

        memset(usage, 0, sizeof(usage));
        return user_copy_out(a1, usage, sizeof(usage)) == 0 ? 0 : -L_EFAULT;
    }
    case 166:
        return 022;
    case 169:
    {
        uint64_t now = timer_read();
        uint64_t tv[2] = {rtc_seconds(), (now % TIMER_FREQ_HZ) / (TIMER_FREQ_HZ / 1000000)};

        return a0 && user_copy_out(a0, tv, sizeof(tv)) != 0 ? -L_EFAULT : 0;
    }
    case 173:
        return 1;
    case 179:
        return do_sysinfo(a0);
    case 214:
        return do_brk(a0);
    case 215:
        return do_munmap(a0, a1);
    case 216:
        return -L_ENOMEM;
    case 226:
        return do_mprotect(a0, a1, a2);
    case 222:
        return do_mmap(a0, a1, a2, a3, (int64_t)frame->a4, frame->a5);
    case 278:
        return do_getrandom(a0, a1);
    case 260:
        return -L_ECHILD;
    case 220:
    case 221:
    case 435:
    case 59:
        warn_unsupported(n);
        return -L_ENOSYS;
    case 258:
    case 291:
    case 293:
        return -L_ENOSYS;
    default:
        warn_unsupported(n);
        return -L_ENOSYS;
    }
}

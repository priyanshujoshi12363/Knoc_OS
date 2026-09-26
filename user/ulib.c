#include "ulib.h"

long syscall(long number, long a0, long a1, long a2)
{
    register long r_a0 asm("a0") = a0;
    register long r_a1 asm("a1") = a1;
    register long r_a2 asm("a2") = a2;
    register long r_a7 asm("a7") = number;

    asm volatile("ecall"
                 : "+r"(r_a0)
                 : "r"(r_a1), "r"(r_a2), "r"(r_a7)
                 : "memory");

    return r_a0;
}

__attribute__((weak)) void exit(int code)
{
    syscall(SYS_EXIT, code, 0, 0);

    while (1)
    {
    }
}

long write(int fd, const void *buffer, unsigned long length)
{
    return syscall(SYS_WRITE, fd, (long)buffer, (long)length);
}

long read(int fd, void *buffer, unsigned long length)
{
    return syscall(SYS_READ, fd, (long)buffer, (long)length);
}

int open(const char *path, int flags)
{
    return (int)syscall(SYS_OPEN, (long)path, flags, 0);
}

int close(int fd)
{
    return (int)syscall(SYS_CLOSE, fd, 0, 0);
}

long seek(int fd, unsigned long offset)
{
    return syscall(SYS_SEEK, fd, (long)offset, 0);
}

int stat(const char *path, file_stat_t *result)
{
    return (int)syscall(SYS_STAT, (long)path, (long)result, 0);
}

int readdir(const char *path, unsigned long index, dir_entry_t *entry)
{
    return (int)syscall(SYS_READDIR, (long)path, (long)index, (long)entry);
}

int mkdir(const char *path)
{
    return (int)syscall(SYS_MKDIR, (long)path, 0, 0);
}

int remove(const char *path)
{
    return (int)syscall(SYS_REMOVE, (long)path, 0, 0);
}

int getpid(void)
{
    return (int)syscall(SYS_GETPID, 0, 0, 0);
}

void yield(void)
{
    syscall(SYS_YIELD, 0, 0, 0);
}

void sleep(unsigned long ticks)
{
    syscall(SYS_SLEEP, (long)ticks, 0, 0);
}

unsigned long uptime(void)
{
    return (unsigned long)syscall(SYS_UPTIME, 0, 0, 0);
}

int spawn(const char *name)
{
    return (int)syscall(SYS_SPAWN, (long)name, 0, 0);
}

int spawn_capture(const char *name, const char *args, int quiet)
{
    return (int)syscall(SYS_SPAWN_CAPTURE, (long)name, (long)args, quiet);
}

long captured(char *buffer, unsigned long length)
{
    return syscall(SYS_CAPTURED, (long)buffer, (long)length, 0);
}

int spawn_args(const char *name, const char *args)
{
    return (int)syscall(SYS_SPAWN, (long)name, (long)args, 0);
}

long getargs(char *buffer, unsigned long length)
{
    return syscall(SYS_GETARGS, (long)buffer, (long)length, 0);
}

int rename(const char *from, const char *to)
{
    return (int)syscall(SYS_RENAME, (long)from, (long)to, 0);
}

int telemetry(unsigned long index, telemetry_sample_t *sample)
{
    return (int)syscall(SYS_TELEMETRY, (long)index, (long)sample, 0);
}

int setclass(int pid, unsigned int process_class)
{
    return (int)syscall(SYS_SETCLASS, (long)pid, (long)process_class, 0);
}

int graph(graph_request_t *request, void *out)
{
    return (int)syscall(SYS_GRAPH, (long)request, (long)out, 0);
}

static void copy_limited(char *to, const char *from)
{
    unsigned long i = 0;

    while (from[i] && i < GRAPH_NAME_MAX - 1)
    {
        to[i] = from[i];
        i++;
    }

    to[i] = 0;
}

int graph_record(unsigned int kind_a, const char *a, unsigned int relation,
                 unsigned int kind_b, const char *b, unsigned int confidence)
{
    graph_request_t request;

    memset(&request, 0, sizeof(request));
    request.op = GRAPH_OP_RECORD;
    request.kind_a = kind_a;
    request.relation = relation;
    request.kind_b = kind_b;
    request.confidence = confidence;
    copy_limited(request.a, a);
    copy_limited(request.b, b);
    return graph(&request, 0);
}

void *mem_alloc(unsigned long bytes)
{
    long result = syscall(SYS_MEM_ALLOC, (long)bytes, 0, 0);

    return is_error(result) ? 0 : (void *)result;
}

long wait(int pid)
{
    return syscall(SYS_WAIT, pid, 0, 0);
}

int ps(unsigned long index, process_info_t *info)
{
    return (int)syscall(SYS_PS, (long)index, (long)info, 0);
}

int kill(int pid)
{
    return (int)syscall(SYS_KILL, pid, 0, 0);
}

int sysinfo(system_info_t *info)
{
    return (int)syscall(SYS_SYSINFO, (long)info, 0, 0);
}

int devinfo(unsigned long index, device_info_t *info)
{
    return (int)syscall(SYS_DEVINFO, (long)index, (long)info, 0);
}

int crashinfo(unsigned long index, crash_info_t *info)
{
    return (int)syscall(SYS_CRASHINFO, (long)index, (long)info, 0);
}

int memcmp_bytes(const void *a, const void *b, unsigned long length)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;

    for (unsigned long i = 0; i < length; i++)
    {
        if (x[i] != y[i])
        {
            return x[i] - y[i];
        }
    }

    return 0;
}

unsigned long strlen(const char *text)
{
    unsigned long length = 0;

    while (text[length])
    {
        length++;
    }

    return length;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }

    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *destination, const char *source)
{
    char *to = destination;

    while ((*to++ = *source++) != 0)
    {
    }

    return destination;
}

/* A decimal number, or -1 if the text isn't one */
long parse_number(const char *text)
{
    long value = 0;

    if (*text == 0)
    {
        return -1;
    }

    while (*text)
    {
        if (*text < '0' || *text > '9')
        {
            return -1;
        }

        value = value * 10 + (*text++ - '0');
    }

    return value;
}

void print_padded(const char *text, int width)
{
    print(text);

    for (int i = (int)strlen(text); i < width; i++)
    {
        print(" ");
    }
}

void print_padded_uint(unsigned long value, int width)
{
    char buffer[21];
    int i = 20;

    buffer[i] = 0;

    do
    {
        buffer[--i] = '0' + (value % 10);
        value /= 10;
    } while (value > 0);

    print_padded(&buffer[i], width);
}

int is_error(long result)
{
    return result < 0 && result >= -64;
}

static int output_fd = FD_STDOUT;

void set_output(int fd)
{
    output_fd = fd;
}

int output(void)
{
    return output_fd;
}

void print(const char *text)
{
    unsigned long length = 0;

    while (text[length])
    {
        length++;
    }

    write(output_fd, text, length);
}

void print_uint(unsigned long value)
{
    char buffer[21];
    int i = 20;

    buffer[i] = 0;

    do
    {
        buffer[--i] = '0' + (value % 10);
        value /= 10;
    } while (value > 0);

    print(&buffer[i]);
}

void print_hex(unsigned long value)
{
    const char *digits = "0123456789ABCDEF";
    char buffer[19];

    buffer[0] = '0';
    buffer[1] = 'x';

    for (int i = 0; i < 16; i++)
    {
        buffer[2 + i] = digits[(value >> ((15 - i) * 4)) & 0xF];
    }

    buffer[18] = 0;
    print(buffer);
}

void *memset(void *destination, int value, unsigned long length)
{
    unsigned char *to = (unsigned char *)destination;

    for (unsigned long i = 0; i < length; i++)
    {
        to[i] = (unsigned char)value;
    }

    return destination;
}

void *memcpy(void *destination, const void *source, unsigned long length)
{
    unsigned char *to = (unsigned char *)destination;
    const unsigned char *from = (const unsigned char *)source;

    for (unsigned long i = 0; i < length; i++)
    {
        to[i] = from[i];
    }

    return destination;
}

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

void exit(int code)
{
    syscall(SYS_EXIT, code, 0, 0);

    while (1)
    {
    }
}

long write(const void *buffer, unsigned long length)
{
    return syscall(SYS_WRITE, (long)buffer, (long)length, 0);
}

long read(void *buffer, unsigned long length)
{
    return syscall(SYS_READ, (long)buffer, (long)length, 0);
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

void *mem_alloc(unsigned long bytes)
{
    long result = syscall(SYS_MEM_ALLOC, (long)bytes, 0, 0);

    return is_error(result) ? 0 : (void *)result;
}

int is_error(long result)
{
    return result < 0 && result >= -64;
}

void print(const char *text)
{
    unsigned long length = 0;

    while (text[length])
    {
        length++;
    }

    write(text, length);
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

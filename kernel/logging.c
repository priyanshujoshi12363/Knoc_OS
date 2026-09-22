#include "logging.h"

#define UART 0x10000000UL

static void uart_putc(char c)
{
    volatile char *uart = (volatile char *)UART;
    *uart = c;
}

static void uart_puts(const char *str)
{
    while (*str)
    {
        uart_putc(*str);
        str++;
    }
}

void log_info(const char *message)
{
    uart_puts("[INFO] ");
    uart_puts(message);
    uart_putc('\n');
}

void log_warn(const char *message)
{
    uart_puts("[WARN] ");
    uart_puts(message);
    uart_putc('\n');
}

static void uart_put_hex(uint64_t value)
{
    const char *digits = "0123456789ABCDEF";

    uart_puts("0x");

    for (int i = 15; i >= 0; i--)
    {
        uart_putc(digits[(value >> (i * 4)) & 0xF]);
    }
}

void log_trap(const char *message)
{
    uart_puts("[TRAP] ");
    uart_puts(message);
    uart_putc('\n');
}

void log_trap_hex(const char *label, uint64_t value)
{
    uart_puts("[TRAP] ");
    uart_puts(label);
    uart_put_hex(value);
    uart_putc('\n');
}

void panic(const char *message)
{
    uart_puts("[PANIC] ");
    uart_puts(message);
    uart_putc('\n');

    while (1)
    {
    }
}
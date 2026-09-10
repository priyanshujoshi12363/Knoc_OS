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

void panic(const char *message)
{
    uart_puts("[PANIC] ");
    uart_puts(message);
    uart_putc('\n');

    while (1)
    {
    }
}
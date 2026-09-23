#include "logging.h"
#include "uart.h"
#include "guardian.h"
#include "trap.h"

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

void log_info_uint(const char *label, uint64_t value)
{
    uart_puts("[INFO] ");
    uart_puts(label);
    uart_put_uint(value);
    uart_putc('\n');
}

void log_info_text(const char *label, const char *text)
{
    uart_puts("[INFO] ");
    uart_puts(label);
    uart_puts(text);
    uart_putc('\n');
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
    asm volatile("csrc sstatus, %0" :: "r"((uint64_t)SSTATUS_SIE));

    uart_puts("[PANIC] ");
    uart_puts(message);
    uart_putc('\n');

    guardian_report_panic(message);

    while (1)
    {
    }
}
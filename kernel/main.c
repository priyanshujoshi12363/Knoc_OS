
#include <stdint.h>

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

static void uart_put_uint64(uint64_t value)
{
    char buffer[21];
    int i = 0;

    if (value == 0)
    {
        uart_putc('0');
        return;
    }

    while (value > 0)
    {
        buffer[i] = '0' + (value % 10);
        value /= 10;
        i++;
    }

    while (i > 0)
    {
        i--;
        uart_putc(buffer[i]);
    }
}


/* Functions provided by memory.c */
extern uint64_t memory_total_bytes(void);
extern uint64_t memory_total_kb(void);
extern uint64_t memory_total_mb(void);
extern uint64_t memory_total_gb(void);
extern uint64_t memory_kernel_bytes(void);
extern uint64_t memory_stack_bytes(void);
extern uint64_t memory_used_bytes(void);
extern uint64_t memory_free_bytes(void);


void kernel_main(void)
{
    uart_puts("KnocOS\n");
    uart_puts("====================\n");
    uart_puts("Memory Information\n");
    uart_puts("====================\n");

    uart_puts("Total RAM: ");
    uart_put_uint64(memory_total_mb());
    uart_puts(" MB\n");

    uart_puts("Kernel: ");
    uart_put_uint64(memory_kernel_bytes());
    uart_puts(" bytes\n");

    uart_puts("Stack: ");
    uart_put_uint64(memory_stack_bytes());
    uart_puts(" bytes\n");

    uart_puts("Used RAM: ");
    uart_put_uint64(memory_used_bytes());
    uart_puts(" bytes\n");

    uart_puts("Free RAM: ");
    uart_put_uint64(memory_free_bytes());
    uart_puts(" bytes\n");

    while (1)
    {
    }
}
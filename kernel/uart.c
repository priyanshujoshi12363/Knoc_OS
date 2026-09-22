#include "uart.h"

#define UART_RBR 0
#define UART_THR 0
#define UART_IER 1
#define UART_FCR 2
#define UART_LCR 3
#define UART_LSR 5

#define UART_IER_RX_AVAILABLE 0x01
#define UART_FCR_ENABLE_AND_CLEAR 0x07
#define UART_LCR_8N1 0x03
#define UART_LSR_DATA_READY 0x01
#define UART_LSR_THR_EMPTY 0x20

#define UART_RX_BUFFER_SIZE 128

static volatile char rx_buffer[UART_RX_BUFFER_SIZE];
static volatile uint32_t rx_head = 0;
static volatile uint32_t rx_tail = 0;

static uint8_t uart_read_reg(uint32_t reg)
{
    volatile uint8_t *uart = (volatile uint8_t *)UART_BASE;
    return uart[reg];
}

static void uart_write_reg(uint32_t reg, uint8_t value)
{
    volatile uint8_t *uart = (volatile uint8_t *)UART_BASE;
    uart[reg] = value;
}

void uart_init(void)
{
    uart_write_reg(UART_IER, 0);
    uart_write_reg(UART_LCR, UART_LCR_8N1);
    uart_write_reg(UART_FCR, UART_FCR_ENABLE_AND_CLEAR);
    uart_write_reg(UART_IER, UART_IER_RX_AVAILABLE);
}

void uart_putc(char c)
{
    while (!(uart_read_reg(UART_LSR) & UART_LSR_THR_EMPTY))
    {
    }

    uart_write_reg(UART_THR, (uint8_t)c);
}

void uart_puts(const char *str)
{
    while (*str)
    {
        uart_putc(*str);
        str++;
    }
}

void uart_put_hex(uint64_t value)
{
    const char *digits = "0123456789ABCDEF";

    uart_puts("0x");

    for (int i = 15; i >= 0; i--)
    {
        uart_putc(digits[(value >> (i * 4)) & 0xF]);
    }
}

void uart_put_uint(uint64_t value)
{
    char buffer[20];
    int i = 0;

    if (value == 0)
    {
        uart_putc('0');
        return;
    }

    while (value > 0)
    {
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    }

    while (i > 0)
    {
        uart_putc(buffer[--i]);
    }
}

void uart_interrupt(void)
{
    while (uart_read_reg(UART_LSR) & UART_LSR_DATA_READY)
    {
        char c = (char)uart_read_reg(UART_RBR);
        uint32_t next = (rx_head + 1) % UART_RX_BUFFER_SIZE;

        if (next != rx_tail)
        {
            rx_buffer[rx_head] = c;
            rx_head = next;
        }
    }
}

int uart_getc(void)
{
    if (rx_head == rx_tail)
    {
        return -1;
    }

    char c = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1) % UART_RX_BUFFER_SIZE;

    return (unsigned char)c;
}

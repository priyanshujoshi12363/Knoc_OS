#include "uart.h"
#include "device.h"
#include "mailbox.h"
#include "timer.h"
#include "process.h"
#include "spinlock.h"

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

#define CONSOLE_LOCK_TIMEOUT (TIMER_FREQ_HZ / 100)

static volatile char rx_buffer[UART_RX_BUFFER_SIZE];
static volatile uint32_t rx_head = 0;
static volatile uint32_t rx_tail = 0;
static int console_line_open = 0;
static char rx_channel;

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

static void uart_init(void)
{
    uart_write_reg(UART_IER, 0);
    uart_write_reg(UART_LCR, UART_LCR_8N1);
    uart_write_reg(UART_FCR, UART_FCR_ENABLE_AND_CLEAR);
    uart_write_reg(UART_IER, UART_IER_RX_AVAILABLE);
}

/* Core 0 and the AI space share the UART. A core owns it for a whole
   line, so lines never mix. After a timeout the line is printed anyway:
   the other core may have crashed while owning it. */
static void console_line_begin(void)
{
    uint64_t start = timer_read();

    while (1)
    {
        uint32_t expected = CONSOLE_FREE;

        if (__atomic_compare_exchange_n(&guardian_mailbox.console_owner,
                                        &expected,
                                        CONSOLE_KERNEL,
                                        0,
                                        __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
        {
            break;
        }

        if (timer_read() - start > CONSOLE_LOCK_TIMEOUT)
        {
            break;
        }
    }

    console_line_open = 1;
}

static void console_line_end(void)
{
    uint32_t expected = CONSOLE_KERNEL;

    __atomic_compare_exchange_n(&guardian_mailbox.console_owner,
                                &expected,
                                CONSOLE_FREE,
                                0,
                                __ATOMIC_RELEASE,
                                __ATOMIC_RELAXED);

    console_line_open = 0;
}

void uart_putc(char c)
{
    if (!console_line_open)
    {
        console_line_begin();
    }

    while (!(uart_read_reg(UART_LSR) & UART_LSR_THR_EMPTY))
    {
    }

    uart_write_reg(UART_THR, (uint8_t)c);

    if (c == '\n')
    {
        console_line_end();
    }
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

static void uart_interrupt(void)
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

    process_wake(&rx_channel);
}

/* Sleep until a key arrives (no polling): the RX interrupt wakes us */
void uart_wait_input(void)
{
    uint64_t enabled = irq_save();

    while (rx_head == rx_tail)
    {
        if (process_can_block())
        {
            process_block(&rx_channel, 0);
        }
        else
        {
            irq_restore(enabled);
            asm volatile("wfi");
            enabled = irq_save();
        }
    }

    irq_restore(enabled);
}

static int uart_getc(void)
{
    if (rx_head == rx_tail)
    {
        return -1;
    }

    char c = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1) % UART_RX_BUFFER_SIZE;

    return (unsigned char)c;
}

static int uart_device_init(device_t *dev)
{
    (void)dev;
    uart_init();
    return 0;
}

static void uart_device_interrupt(device_t *dev)
{
    (void)dev;
    uart_interrupt();
}

static int64_t uart_device_read(device_t *dev, void *buffer, uint64_t length)
{
    (void)dev;

    char *bytes = (char *)buffer;
    uint64_t count = 0;

    while (count < length)
    {
        int c = uart_getc();

        if (c < 0)
        {
            break;
        }

        bytes[count++] = (char)c;
    }

    return (int64_t)count;
}

static int64_t uart_device_write(device_t *dev, const void *buffer, uint64_t length)
{
    (void)dev;

    const char *bytes = (const char *)buffer;

    for (uint64_t i = 0; i < length; i++)
    {
        uart_putc(bytes[i]);
    }

    return (int64_t)length;
}

static device_t uart_device = {
    .name = "uart0",
    .irq = UART_IRQ,
    .init = uart_device_init,
    .interrupt = uart_device_interrupt,
    .read = uart_device_read,
    .write = uart_device_write,
};

void uart_register(void)
{
    device_register(&uart_device);
}

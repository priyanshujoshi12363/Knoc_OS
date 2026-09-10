#include <stdint.h>
#include "page.h"

#define RAM_START 0x80000000UL
#define RAM_END   0x88000000UL

#define TOTAL_PAGES ((RAM_END - RAM_START) / PAGE_SIZE)

static unsigned char page_bitmap[TOTAL_PAGES / 8];

static unsigned long first_free_page;

extern char kernel_start;
extern char kernel_end;
extern char stack_bottom;
extern char stack_top;

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

static void uart_put_uint(unsigned long value)
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

static void page_set_used(unsigned long page_number)
{
    page_bitmap[page_number / 8] |=
        (1 << (page_number % 8));
}

static void page_set_free(unsigned long page_number)
{
    page_bitmap[page_number / 8] &=
        ~(1 << (page_number % 8));
}

static int page_is_used(unsigned long page_number)
{
    return page_bitmap[page_number / 8] &
           (1 << (page_number % 8));
}

void page_init(void)
{
    unsigned long i;

    /*
     * Safety first:
     * Mark every physical page as USED.
     */
    for (i = 0; i < TOTAL_PAGES; i++)
    {
        page_set_used(i);
    }

    /*
     * The stack may end in the middle of a page.
     * Align upward to the next complete 4 KiB page.
     */
    uintptr_t usable_start =
        ((uintptr_t)&stack_top + PAGE_SIZE - 1)
        & ~(PAGE_SIZE - 1);

    uintptr_t usable_end = RAM_END;

    first_free_page =
        (usable_start - RAM_START) / PAGE_SIZE;

    unsigned long last_free_page =
        (usable_end - RAM_START) / PAGE_SIZE;

    /*
     * Everything after the kernel + stack is FREE.
     */
    for (i = first_free_page; i < last_free_page; i++)
    {
        page_set_free(i);
    }
}
void page_debug(void)
{
    unsigned long i = 0;

    uart_puts("\n");
    uart_puts("================================\n");
    uart_puts("        KnocOS Page Manager\n");
    uart_puts("================================\n");

    uart_puts("Page size  : ");
    uart_put_uint(PAGE_SIZE);
    uart_puts(" bytes\n");

    uart_puts("Total pages: ");
    uart_put_uint(TOTAL_PAGES);
    uart_puts("\n");

    uart_puts("--------------------------------\n");
    uart_puts("Memory Map\n");
    uart_puts("--------------------------------\n");

    while (i < TOTAL_PAGES)
    {
        unsigned long start = i;
        int used = page_is_used(i);

        /*
         * Find the end of this consecutive
         * USED or FREE range.
         */
        while (i < TOTAL_PAGES &&
               page_is_used(i) == used)
        {
            i++;
        }

        unsigned long end = i - 1;
        unsigned long count = end - start + 1;

        uart_puts("Pages ");
        uart_put_uint(start);
        uart_puts(" - ");
        uart_put_uint(end);

        uart_puts(" : ");

        if (used)
        {
            uart_puts("USED");
        }
        else
        {
            uart_puts("FREE");
        }

        uart_puts(" (");
        uart_put_uint(count);
        uart_puts(" pages)\n");
    }

    uart_puts("================================\n");
}

void *page_alloc(void)
{
    unsigned long i;

    for (i = first_free_page; i < TOTAL_PAGES; i++)
    {
        if (!page_is_used(i))
        {
            page_set_used(i);

            return (void *)(RAM_START + (i * PAGE_SIZE));
        }
    }

    return (void *)0;
}

void page_free(void *address)
{
    uintptr_t addr = (uintptr_t)address;

    if (addr < RAM_START || addr >= RAM_END)
    {
        return;
    }

    if ((addr - RAM_START) % PAGE_SIZE != 0)
    {
        return;
    }

    unsigned long page_number =
        (addr - RAM_START) / PAGE_SIZE;

    if (page_number < first_free_page)
    {
        return;
    }

    page_set_free(page_number);
}
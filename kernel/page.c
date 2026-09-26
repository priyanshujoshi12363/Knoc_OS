#include <stdint.h>
#include "page.h"
#include "uart.h"
#include "aispace.h"
#include "spinlock.h"

/* Buddy allocator. Free memory is kept as blocks of 2^order pages
   (order 0 = 4 KiB ... order 18 = 1 GiB), each aligned to its own size.
   A block's "buddy" is the neighbour it was split from: index ^ 2^order.
   When both are free they merge back into one bigger block. */

#define PAGE_META_FREE 0x80
#define PAGE_META_HEAD 0x40
#define PAGE_META_ORDER 0x1F

#define PAGE_RESERVED_MAX 3

typedef struct free_block
{
    struct free_block *next;
    struct free_block *prev;
} free_block_t;

typedef struct page_range
{
    uintptr_t start;
    uintptr_t end;
} page_range_t;

static uintptr_t ram_start;
static uintptr_t ram_end;
static unsigned long total_pages;
static unsigned long free_pages;
static uint8_t *page_meta;
static free_block_t *free_lists[PAGE_MAX_ORDER + 1];
static spinlock_t page_lock = SPINLOCK_INIT;

extern char stack_top;
extern char cpu_stacks_end;

static uintptr_t align_up(uintptr_t value, uintptr_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static uintptr_t align_down(uintptr_t value, uintptr_t alignment)
{
    return value & ~(alignment - 1);
}

static uintptr_t page_address(unsigned long index)
{
    return ram_start + index * PAGE_SIZE;
}

static unsigned long page_index(uintptr_t address)
{
    return (address - ram_start) / PAGE_SIZE;
}

static void list_push(unsigned int order, unsigned long index)
{
    free_block_t *block = (free_block_t *)page_address(index);

    block->prev = 0;
    block->next = free_lists[order];

    if (free_lists[order] != 0)
    {
        free_lists[order]->prev = block;
    }

    free_lists[order] = block;
    page_meta[index] = PAGE_META_FREE | order;
}

static void list_remove(unsigned int order, unsigned long index)
{
    free_block_t *block = (free_block_t *)page_address(index);

    if (block->prev != 0)
    {
        block->prev->next = block->next;
    }
    else
    {
        free_lists[order] = block->next;
    }

    if (block->next != 0)
    {
        block->next->prev = block->prev;
    }

    page_meta[index] = 0;
}

static void add_free_range(uintptr_t start, uintptr_t end)
{
    unsigned long index = page_index(align_up(start, PAGE_SIZE));
    unsigned long last = page_index(align_down(end, PAGE_SIZE));

    while (index < last)
    {
        unsigned int order = PAGE_MAX_ORDER;

        while (order > 0 &&
               ((index & ((1UL << order) - 1)) != 0 ||
                index + (1UL << order) > last))
        {
            order--;
        }

        list_push(order, index);
        free_pages += 1UL << order;
        index += 1UL << order;
    }
}

int page_init(uintptr_t start, uint64_t size, uintptr_t dtb_start, uint64_t dtb_size)
{
    ram_start = start;
    ram_end = start + size;
    total_pages = size / PAGE_SIZE;
    free_pages = 0;

    for (unsigned int order = 0; order <= PAGE_MAX_ORDER; order++)
    {
        free_lists[order] = 0;
    }

    /* One metadata byte per page, placed right after the boot stack */
    page_meta = (uint8_t *)align_up((uintptr_t)&cpu_stacks_end, PAGE_SIZE);
    uintptr_t meta_end = align_up((uintptr_t)page_meta + total_pages, PAGE_SIZE);

    if (meta_end >= AISPACE_BASE || meta_end >= ram_end)
    {
        return -1;
    }

    for (unsigned long i = 0; i < total_pages; i++)
    {
        page_meta[i] = 0;
    }

    page_range_t reserved[PAGE_RESERVED_MAX] = {
        {ram_start, meta_end},
        {AISPACE_BASE, AISPACE_BASE + AISPACE_SIZE},
        {align_down(dtb_start, PAGE_SIZE), align_up(dtb_start + dtb_size, PAGE_SIZE)},
    };

    for (int i = 0; i < PAGE_RESERVED_MAX; i++)
    {
        for (int j = i + 1; j < PAGE_RESERVED_MAX; j++)
        {
            if (reserved[j].start < reserved[i].start)
            {
                page_range_t swap = reserved[i];
                reserved[i] = reserved[j];
                reserved[j] = swap;
            }
        }
    }

    uintptr_t cursor = ram_start;

    for (int i = 0; i < PAGE_RESERVED_MAX; i++)
    {
        uintptr_t hole_end = reserved[i].start < ram_end ? reserved[i].start : ram_end;

        if (hole_end > cursor)
        {
            add_free_range(cursor, hole_end);
        }

        if (reserved[i].end > cursor)
        {
            cursor = reserved[i].end;
        }
    }

    if (cursor < ram_end)
    {
        add_free_range(cursor, ram_end);
    }

    return 0;
}

static void *alloc_locked(unsigned int order)
{
    unsigned int current = order;

    while (current <= PAGE_MAX_ORDER && free_lists[current] == 0)
    {
        current++;
    }

    if (current > PAGE_MAX_ORDER)
    {
        return 0;
    }

    unsigned long index = page_index((uintptr_t)free_lists[current]);
    list_remove(current, index);

    while (current > order)
    {
        current--;
        list_push(current, index + (1UL << current));
    }

    page_meta[index] = PAGE_META_HEAD | order;
    free_pages -= 1UL << order;

    return (void *)page_address(index);
}

void *page_alloc_order(unsigned int order)
{
    if (order > PAGE_MAX_ORDER || page_meta == 0)
    {
        return 0;
    }

    uint64_t interrupts = spin_lock(&page_lock);
    void *address = alloc_locked(order);
    spin_unlock(&page_lock, interrupts);

    return address;
}

void *page_alloc(void)
{
    return page_alloc_order(0);
}

void *page_alloc_contiguous(uint64_t bytes)
{
    unsigned int order = 0;

    while (order <= PAGE_MAX_ORDER && (PAGE_SIZE << order) < bytes)
    {
        order++;
    }

    return page_alloc_order(order);
}

void page_free(void *address)
{
    uintptr_t addr = (uintptr_t)address;

    if (page_meta == 0 || addr < ram_start || addr >= ram_end ||
        (addr - ram_start) % PAGE_SIZE != 0)
    {
        return;
    }

    uint64_t interrupts = spin_lock(&page_lock);

    unsigned long index = page_index(addr);

    if (!(page_meta[index] & PAGE_META_HEAD))
    {
        spin_unlock(&page_lock, interrupts);
        return;
    }

    unsigned int order = page_meta[index] & PAGE_META_ORDER;

    page_meta[index] = 0;
    free_pages += 1UL << order;

    while (order < PAGE_MAX_ORDER)
    {
        unsigned long buddy = index ^ (1UL << order);

        if (buddy >= total_pages ||
            page_meta[buddy] != (PAGE_META_FREE | order))
        {
            break;
        }

        list_remove(order, buddy);

        if (buddy < index)
        {
            index = buddy;
        }

        order++;
    }

    list_push(order, index);

    spin_unlock(&page_lock, interrupts);
}

unsigned long page_total(void)
{
    return total_pages;
}

unsigned long page_used(void)
{
    return total_pages - free_pages;
}

unsigned long page_free_count(void)
{
    return free_pages;
}

unsigned long page_largest_free(void)
{
    for (int order = PAGE_MAX_ORDER; order >= 0; order--)
    {
        if (free_lists[order] != 0)
        {
            return 1UL << order;
        }
    }

    return 0;
}

uintptr_t page_ram_start(void)
{
    return ram_start;
}

uintptr_t page_ram_end(void)
{
    return ram_end;
}

void page_debug(void)
{
    uart_puts("[INFO] Free blocks by size:\n");

    for (unsigned int order = 0; order <= PAGE_MAX_ORDER; order++)
    {
        unsigned long count = 0;

        for (free_block_t *block = free_lists[order]; block != 0; block = block->next)
        {
            count++;
        }

        if (count == 0)
        {
            continue;
        }

        uart_puts("       ");
        uart_put_uint((PAGE_SIZE << order) / 1024);
        uart_puts(" KiB x ");
        uart_put_uint(count);
        uart_putc('\n');
    }
}

#include <stdint.h>
#include "logging.h"
#include "page.h"
#include "vm.h"
#include "heap.h"

void kernel_main(void)
{
    log_info("KnocOS starting");

    page_init();

    log_info("Page memory initialized");

    vm_init();

    log_info("Virtual memory initialized");

    log_info("Kernel page tables ready");

    vm_debug(0x80000000UL);

    heap_init();

    log_info("Kernel heap mapping prepared");

    unsigned long pages_after_heap_init =
        page_used();

    log_info("Initial heap physical memory verified");

    log_info("Enabling Sv39");

    vm_enable();

    log_info("Sv39 enabled");

    heap_activate();

    log_info("Kernel heap activated");

    void *block_a = kmalloc(3000);

    if (block_a == 0)
    {
        panic("Allocation A failed");
    }

    log_info("Allocation A successful");

    unsigned long pages_after_allocation_a =
        page_used();

    if (pages_after_allocation_a != pages_after_heap_init)
    {
        panic("Unexpected page allocation during A");
    }

    void *block_b = kmalloc(3000);

    if (block_b == 0)
    {
        panic("Allocation B failed");
    }

    log_info("Allocation B successful");

    unsigned long pages_after_growth =
        page_used();

    if (pages_after_growth != pages_after_allocation_a + 1)
    {
        panic("Heap page growth failed");
    }

    log_info("Automatic physical page growth verified");

    volatile uint64_t *value_a =
        (volatile uint64_t *)block_a;

    volatile uint64_t *value_b =
        (volatile uint64_t *)block_b;

    *value_a = 0xAAAAAAAAAAAAAAAAULL;
    *value_b = 0xBBBBBBBBBBBBBBBBULL;

    if (*value_a != 0xAAAAAAAAAAAAAAAAULL)
    {
        panic("Allocation A memory test failed");
    }

    if (*value_b != 0xBBBBBBBBBBBBBBBBULL)
    {
        panic("Allocation B memory test failed");
    }

    log_info("Multiple-page heap allocation verified");

    kfree(block_a);

    log_info("Allocation A freed");

    kfree(block_b);

    log_info("Allocation B freed");

    log_info("Kernel heap automatic growth test passed");

    while (1)
    {
    }
}
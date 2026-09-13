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

    log_info("Enabling Sv39");

    vm_enable();

    log_info("Sv39 enabled");

    heap_activate();

    log_info("Kernel heap activated");

    void *block_a = kmalloc(100);

    if (block_a == 0)
    {
        panic("Allocation A failed");
    }

    log_info("Allocation A successful");

    void *block_b = kmalloc(200);

    if (block_b == 0)
    {
        panic("Allocation B failed");
    }

    log_info("Allocation B successful");

    void *block_c = kmalloc(300);

    if (block_c == 0)
    {
        panic("Allocation C failed");
    }

    log_info("Allocation C successful");

    volatile uint64_t *value_a =
        (volatile uint64_t *)block_a;

    volatile uint64_t *value_b =
        (volatile uint64_t *)block_b;

    volatile uint64_t *value_c =
        (volatile uint64_t *)block_c;

    *value_a = 0xAAAAAAAAAAAAAAAAULL;
    *value_b = 0xBBBBBBBBBBBBBBBBULL;
    *value_c = 0xCCCCCCCCCCCCCCCCULL;

    if (*value_a != 0xAAAAAAAAAAAAAAAAULL)
    {
        panic("Allocation A memory test failed");
    }

    if (*value_b != 0xBBBBBBBBBBBBBBBBULL)
    {
        panic("Allocation B memory test failed");
    }

    if (*value_c != 0xCCCCCCCCCCCCCCCCULL)
    {
        panic("Allocation C memory test failed");
    }

    log_info("Multiple heap allocations verified");

    kfree(block_b);

    log_info("Allocation B freed");

    kfree(block_a);

    log_info("Allocation A freed");

    kfree(block_c);

    log_info("Allocation C freed");

    log_info("Kernel heap splitting test passed");

    while (1)
    {
    }
}
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

    void *test_memory = kmalloc(100);

    if (test_memory == 0)
    {
        panic("Kernel heap allocation failed");
    }

    volatile uint64_t *test_value =
        (volatile uint64_t *)test_memory;

    *test_value = 0x4B4E4F434F534845ULL;

    if (*test_value != 0x4B4E4F434F534845ULL)
    {
        panic("Kernel heap allocation test failed");
    }

    log_info("Kernel heap allocation verified");

    kfree(test_memory);

    log_info("Kernel heap free verified");

    while (1)
    {
    }
}
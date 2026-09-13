#include "logging.h"
#include "page.h"
#include "vm.h"

void kernel_main(void)
{
    log_info("KnocOS starting");

    page_init();

    log_info("Page memory initialized");

    vm_init();

    log_info("Virtual memory initialized");

    log_info("Kernel page tables ready");

    vm_debug(0x80000000UL);

    uintptr_t test_virtual = 0x40000000UL;
    uintptr_t test_physical = 0x80000000UL;

    if (vm_map(test_virtual,
               test_physical,
               PTE_R | PTE_W) != 0)
    {
        panic("Hardware translation test mapping failed");
    }

    log_info("Hardware translation test mapping ready");

    vm_debug(test_virtual);

    log_info("Enabling Sv39");

    vm_enable();

    log_info("Sv39 enabled");

    volatile uint64_t *test_address =
        (volatile uint64_t *)test_virtual;

    *test_address = 0x4B4E4F434F53ULL;

    if (*test_address != 0x4B4E4F434F53ULL)
    {
        panic("Hardware translation test failed");
    }

    log_info("Hardware Sv39 translation verified");

    while (1)
    {
    }
}
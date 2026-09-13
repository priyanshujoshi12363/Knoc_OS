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

    if (vm_map(0x80000000UL, 0x80000000UL, PTE_R | PTE_W | PTE_X) != 0)
    {
        panic("Virtual mapping failed");
    }

    log_info("Kernel page mapped");

    vm_debug(0x80000000UL);

    while (1)
    {
    }
}
#include "logging.h"
#include "page.h"

void kernel_main(void)
{
    log_info("KnocOS starting");

    page_init();

    log_info("Page memory initialized");

    while (1)
    {
    }
}
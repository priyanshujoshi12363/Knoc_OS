#include "logging.h"
#include "page.h"

void kernel_main(void)
{
    log_info("KnocOS starting");

    page_init();

    log_info("Page memory initialized");

    page_debug();

    void *page = page_alloc();

    if (page != (void *)0)
    {
        log_info("Page allocated");

        page_debug();

        page_free(page);

        log_info("Page freed");

        page_debug();
    }
    else
    {
        log_info("Page allocation failed");
    }

    while (1)
    {
    }
}
#include "timer.h"

#define CLINT_MTIME 0x0200BFF8UL
#define CLINT_MTIMECMP 0x02004000UL

uint64_t timer_read(void)
{
    volatile uint64_t *mtime =
        (volatile uint64_t *)CLINT_MTIME;

    return *mtime;
}

void timer_set_next(uint64_t value)
{
    volatile uint64_t *mtimecmp =
        (volatile uint64_t *)CLINT_MTIMECMP;

    *mtimecmp = value;
}
void timer_enable(void)
{
    uint64_t value;

    __asm__ volatile(
        "csrr %0, mie"
        : "=r"(value)
    );

    value |= (1UL << 7);

    __asm__ volatile(
        "csrw mie, %0"
        :
        : "r"(value)
    );
}
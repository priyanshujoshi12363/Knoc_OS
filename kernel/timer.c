#include "timer.h"

#define CLINT_MTIMECMP 0x02004000UL

static volatile uint64_t ticks = 0;

uint64_t timer_read(void)
{
    uint64_t value;

    asm volatile(
        "rdtime %0"
        : "=r"(value)
    );

    return value;
}

void timer_set_next(uint64_t value)
{
    volatile uint64_t *mtimecmp =
        (volatile uint64_t *)CLINT_MTIMECMP;

    *mtimecmp = value;
}

void timer_interrupt(void)
{
    volatile uint64_t *mtimecmp =
        (volatile uint64_t *)CLINT_MTIMECMP;

    uint64_t next = *mtimecmp + TIMER_INTERVAL;

    *mtimecmp = next;

    ticks++;
}

uint64_t timer_ticks(void)
{
    return ticks;
}
#include "timer.h"

#define CLINT_MTIME 0x0200BFF8UL
#define CLINT_MTIMECMP 0x02004000UL
#define TIMER_INTERVAL 100000ULL

static uint64_t ticks = 0;

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

void timer_interrupt(void)
{
    ticks++;

    timer_set_next(timer_read() + TIMER_INTERVAL);
}

uint64_t timer_ticks(void)
{
    return ticks;
}
#include "timer.h"
#include "trap.h"

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

void timer_interrupt(void)
{
    volatile uint64_t *mtimecmp =
        (volatile uint64_t *)(uintptr_t)CLINT_MTIMECMP;

    uint64_t next = *mtimecmp + TIMER_INTERVAL;

    *mtimecmp = next;

    asm volatile("csrs mip, %0" :: "r"(SIP_SSIP));
}

void timer_tick(void)
{
    ticks++;
}

uint64_t timer_ticks(void)
{
    return ticks;
}
#include "timer.h"
#include "trap.h"
#include "mailbox.h"

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
    uint64_t hart;

    asm volatile("csrr %0, mhartid" : "=r"(hart));

    volatile uint64_t *mtimecmp =
        (volatile uint64_t *)(uintptr_t)(CLINT_MTIMECMP + hart * 8);

    uint64_t next = *mtimecmp + TIMER_INTERVAL;
    uint64_t now = *(volatile uint64_t *)(uintptr_t)CLINT_MTIME;

    while (next <= now)
    {
        next += TIMER_INTERVAL;
    }

    *mtimecmp = next;

    if (hart == 0)
    {
        uint64_t mepc;
        asm volatile("csrr %0, mepc" : "=r"(mepc));
        guardian_mailbox.last_kernel_pc = mepc;
    }

    asm volatile("csrs mip, %0" :: "r"(SIP_SSIP));
}

void timer_tick(uint64_t count)
{
    ticks += count;
}

uint64_t timer_ticks(void)
{
    return ticks;
}
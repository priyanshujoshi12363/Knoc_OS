#include "plic.h"
#include "mmio.h"

#define PLIC_PRIORITY(irq) (PLIC_BASE + (irq) * 4)
#define PLIC_SUPERVISOR_ENABLE (PLIC_BASE + 0x2080)
#define PLIC_SUPERVISOR_THRESHOLD (PLIC_BASE + 0x201000)
#define PLIC_SUPERVISOR_CLAIM (PLIC_BASE + 0x201004)

#define PLIC_DEFAULT_PRIORITY 1

static uint32_t plic_read(uintptr_t address)
{
    return *(volatile uint32_t *)MMIO(address);
}

static void plic_write(uintptr_t address, uint32_t value)
{
    *(volatile uint32_t *)MMIO(address) = value;
}

void plic_init(void)
{
    plic_write(PLIC_SUPERVISOR_ENABLE, 0);
    plic_write(PLIC_SUPERVISOR_THRESHOLD, 0);
}

void plic_enable(uint32_t irq)
{
    plic_write(PLIC_PRIORITY(irq), PLIC_DEFAULT_PRIORITY);

    uint32_t enabled = plic_read(PLIC_SUPERVISOR_ENABLE);
    plic_write(PLIC_SUPERVISOR_ENABLE, enabled | (1U << irq));

    /* After a warm restart, an interrupt claimed by the crashed kernel
       is still in progress and would block this IRQ forever */
    plic_complete(irq);
}

void plic_disable(uint32_t irq)
{
    uint32_t enabled = plic_read(PLIC_SUPERVISOR_ENABLE);
    plic_write(PLIC_SUPERVISOR_ENABLE, enabled & ~(1U << irq));
}

uint32_t plic_claim(void)
{
    return plic_read(PLIC_SUPERVISOR_CLAIM);
}

void plic_complete(uint32_t irq)
{
    plic_write(PLIC_SUPERVISOR_CLAIM, irq);
}

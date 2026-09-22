#include <stdint.h>
#include "power.h"

#define POWER_OFF_VALUE 0x5555
#define POWER_REBOOT_VALUE 0x7777

static void power_write(uint32_t value)
{
    volatile uint32_t *power = (volatile uint32_t *)POWER_BASE;
    *power = value;

    while (1)
    {
    }
}

void power_off(void)
{
    power_write(POWER_OFF_VALUE);
}

void power_reboot(void)
{
    power_write(POWER_REBOOT_VALUE);
}

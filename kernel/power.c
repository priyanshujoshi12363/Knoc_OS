#include <stdint.h>
#include "power.h"
#include "device.h"

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

static int64_t power_device_write(device_t *dev, const void *buffer, uint64_t length)
{
    (void)dev;

    if (length < 1)
    {
        return -1;
    }

    uint8_t command = *(const uint8_t *)buffer;

    if (command == POWER_COMMAND_OFF)
    {
        power_off();
    }

    if (command == POWER_COMMAND_REBOOT)
    {
        power_reboot();
    }

    return -1;
}

static device_t power_device = {
    .name = "power0",
    .irq = DEVICE_NO_IRQ,
    .write = power_device_write,
};

void power_register(void)
{
    device_register(&power_device);
}

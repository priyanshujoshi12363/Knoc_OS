#include <stdint.h>
#include "faulty.h"
#include "device.h"

/* A test driver with a bug on purpose: every write uses a bad pointer.
   It lets the tests check that a driver crash is contained and that the
   AI space disables the driver. */

#define FAULTY_BAD_ADDRESS 0x40000000UL

static int64_t faulty_write(device_t *dev, const void *buffer, uint64_t length)
{
    (void)dev;
    (void)buffer;

    *(volatile uint64_t *)FAULTY_BAD_ADDRESS = 1;

    return (int64_t)length;
}

static device_t faulty_device = {
    .name = "faulty0",
    .irq = DEVICE_NO_IRQ,
    .write = faulty_write,
};

void faulty_register(void)
{
    device_register(&faulty_device);
}

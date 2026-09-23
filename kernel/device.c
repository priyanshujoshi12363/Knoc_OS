#include "device.h"
#include "plic.h"
#include "uart.h"

static device_t *devices[DEVICE_MAX];
static uint32_t registered_count = 0;

static int names_equal(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }

    return *a == *b;
}

static void print_device(const char *status, device_t *dev)
{
    uart_puts("[INFO] ");
    uart_puts(status);
    uart_puts(dev->name);

    if (dev->irq != DEVICE_NO_IRQ)
    {
        uart_puts(" (IRQ ");
        uart_put_uint(dev->irq);
        uart_puts(")");
    }

    uart_putc('\n');
}

int device_register(device_t *dev)
{
    if (dev == 0 || dev->name == 0)
    {
        return -1;
    }

    if (registered_count >= DEVICE_MAX)
    {
        return -1;
    }

    if (device_find(dev->name) != 0)
    {
        return -1;
    }

    dev->ready = 0;
    devices[registered_count] = dev;
    registered_count++;

    return 0;
}

void device_init_all(void)
{
    for (uint32_t i = 0; i < registered_count; i++)
    {
        device_t *dev = devices[i];

        if (dev->init != 0 && dev->init(dev) != 0)
        {
            print_device("Device failed: ", dev);
            continue;
        }

        if (dev->irq != DEVICE_NO_IRQ)
        {
            plic_enable(dev->irq);
        }

        dev->ready = 1;
        print_device("Device ready: ", dev);
    }
}

device_t *device_find(const char *name)
{
    if (name == 0)
    {
        return 0;
    }

    for (uint32_t i = 0; i < registered_count; i++)
    {
        if (names_equal(devices[i]->name, name))
        {
            return devices[i];
        }
    }

    return 0;
}

uint32_t device_count(void)
{
    return registered_count;
}

int64_t device_read(device_t *dev, void *buffer, uint64_t length)
{
    if (dev == 0 || !dev->ready || dev->read == 0 || buffer == 0)
    {
        return -1;
    }

    return dev->read(dev, buffer, length);
}

int64_t device_write(device_t *dev, const void *buffer, uint64_t length)
{
    if (dev == 0 || !dev->ready || dev->write == 0 || buffer == 0)
    {
        return -1;
    }

    return dev->write(dev, buffer, length);
}

int device_handle_irq(uint32_t irq)
{
    for (uint32_t i = 0; i < registered_count; i++)
    {
        device_t *dev = devices[i];

        if (dev->irq == irq && dev->ready && dev->interrupt != 0)
        {
            dev->interrupt(dev);
            return 0;
        }
    }

    return -1;
}

void device_list(void)
{
    uart_puts("[INFO] Devices: ");
    uart_put_uint(registered_count);
    uart_putc('\n');

    for (uint32_t i = 0; i < registered_count; i++)
    {
        device_t *dev = devices[i];

        uart_puts("       ");
        uart_puts(dev->name);

        if (dev->irq != DEVICE_NO_IRQ)
        {
            uart_puts("  IRQ ");
            uart_put_uint(dev->irq);
        }

        uart_puts(dev->ready ? "  ready" : "  failed");
        uart_putc('\n');
    }
}

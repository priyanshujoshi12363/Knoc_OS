#include "device.h"
#include "plic.h"
#include "uart.h"
#include "process.h"
#include "guardian.h"

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
    dev->disabled = 0;
    devices[registered_count] = dev;
    registered_count++;

    return 0;
}

void device_init_all(void)
{
    for (uint32_t i = 0; i < registered_count; i++)
    {
        device_t *dev = devices[i];

        if (guardian_driver_disabled(dev->name))
        {
            dev->disabled = 1;
            uart_puts("[WARN] Device disabled by the AI space: ");
            uart_puts(dev->name);
            uart_puts(" (it crashed before)\n");
            continue;
        }

        const char *previous = process_driver_enter(dev->name);
        int result = dev->init != 0 ? dev->init(dev) : 0;
        process_driver_leave(previous);

        if (result == DEVICE_ABSENT)
        {
            print_device("Device not present: ", dev);
            continue;
        }

        if (result != 0)
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

device_t *device_at(uint32_t index)
{
    return index < registered_count ? devices[index] : 0;
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

    const char *previous = process_driver_enter(dev->name);
    int64_t result = dev->read(dev, buffer, length);
    process_driver_leave(previous);

    return result;
}

int64_t device_write(device_t *dev, const void *buffer, uint64_t length)
{
    if (dev == 0 || !dev->ready || dev->write == 0 || buffer == 0)
    {
        return -1;
    }

    const char *previous = process_driver_enter(dev->name);
    int64_t result = dev->write(dev, buffer, length);
    process_driver_leave(previous);

    return result;
}

int device_read_block(device_t *dev, uint64_t block, void *buffer)
{
    if (dev == 0 || !dev->ready || dev->read_block == 0 || buffer == 0)
    {
        return -1;
    }

    if (block >= dev->block_count)
    {
        return -1;
    }

    const char *previous = process_driver_enter(dev->name);
    int result = dev->read_block(dev, block, buffer);
    process_driver_leave(previous);

    return result;
}

int device_write_block(device_t *dev, uint64_t block, const void *buffer)
{
    if (dev == 0 || !dev->ready || dev->write_block == 0 || buffer == 0)
    {
        return -1;
    }

    if (block >= dev->block_count)
    {
        return -1;
    }

    const char *previous = process_driver_enter(dev->name);
    int result = dev->write_block(dev, block, buffer);
    process_driver_leave(previous);

    return result;
}

/* Several blocks in one request when the driver supports it */
int device_read_blocks(device_t *dev, uint64_t block, uint64_t count, void *buffer)
{
    if (dev == 0 || !dev->ready || buffer == 0 || block + count > dev->block_count)
    {
        return -1;
    }

    if (dev->read_blocks == 0)
    {
        for (uint64_t i = 0; i < count; i++)
        {
            if (device_read_block(dev, block + i, (uint8_t *)buffer + i * dev->block_size) != 0)
            {
                return -1;
            }
        }

        return 0;
    }

    const char *previous = process_driver_enter(dev->name);
    int result = dev->read_blocks(dev, block, count, buffer);
    process_driver_leave(previous);

    return result;
}

int device_write_blocks(device_t *dev, uint64_t block, uint64_t count, const void *buffer)
{
    if (dev == 0 || !dev->ready || buffer == 0 || block + count > dev->block_count)
    {
        return -1;
    }

    if (dev->write_blocks == 0)
    {
        for (uint64_t i = 0; i < count; i++)
        {
            if (device_write_block(dev, block + i, (const uint8_t *)buffer + i * dev->block_size) != 0)
            {
                return -1;
            }
        }

        return 0;
    }

    const char *previous = process_driver_enter(dev->name);
    int result = dev->write_blocks(dev, block, count, buffer);
    process_driver_leave(previous);

    return result;
}

int device_handle_irq(uint32_t irq)
{
    for (uint32_t i = 0; i < registered_count; i++)
    {
        device_t *dev = devices[i];

        if (dev->irq == irq && dev->ready && dev->interrupt != 0)
        {
            const char *previous = process_driver_enter(dev->name);
            dev->interrupt(dev);
            process_driver_leave(previous);
            return 0;
        }
    }

    return -1;
}

int device_disable(device_t *dev)
{
    if (dev == 0)
    {
        return -1;
    }

    dev->ready = 0;
    dev->disabled = 1;

    if (dev->irq != DEVICE_NO_IRQ)
    {
        plic_disable(dev->irq);
    }

    return 0;
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

        if (dev->block_count != 0)
        {
            uart_puts("  ");
            uart_put_uint(dev->block_count);
            uart_puts(" blocks");
        }

        uart_puts(dev->ready ? "  ready" : dev->disabled ? "  disabled" : "  failed");
        uart_putc('\n');
    }
}

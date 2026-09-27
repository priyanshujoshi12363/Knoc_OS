#include "virtio_input.h"
#include "virtio.h"
#include "mmio.h"
#include "device.h"
#include "page.h"
#include "input.h"

#define DEVICE_ID_INPUT 18
#define QUEUE_SIZE 64
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define REL_WHEEL 8

typedef struct input_raw
{
    uint16_t type;
    uint16_t code;
    uint32_t value;
} input_raw_t;

typedef struct input_avail
{
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QUEUE_SIZE];
    uint16_t used_event;
} input_avail_t;

typedef struct input_used
{
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[QUEUE_SIZE];
    uint16_t avail_event;
} input_used_t;

typedef struct input_port
{
    device_t device;
    uintptr_t base;
    volatile virtq_desc_t *desc;
    volatile input_avail_t *avail;
    volatile input_used_t *used;
    input_raw_t *events;
    uint16_t used_seen;
} input_port_t;

static uint32_t reg_read(input_port_t *port, uint32_t reg)
{
    return *(volatile uint32_t *)MMIO(port->base + reg);
}

static void reg_write(input_port_t *port, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)MMIO(port->base + reg) = value;
}

static void zero(void *memory)
{
    uint64_t *words = memory;

    for (uint64_t i = 0; i < PAGE_SIZE / 8; i++)
    {
        words[i] = 0;
    }
}

static void set_address(input_port_t *port, uint32_t low, uint32_t high, uintptr_t address)
{
    reg_write(port, low, (uint32_t)address);
    reg_write(port, high, (uint32_t)(address >> 32));
}

static int virtio_input_init(device_t *dev)
{
    input_port_t *port = (input_port_t *)dev;

    if (reg_read(port, VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MAGIC ||
        reg_read(port, VIRTIO_MMIO_VERSION) != VIRTIO_VERSION_MODERN ||
        reg_read(port, VIRTIO_MMIO_DEVICE_ID) != DEVICE_ID_INPUT)
    {
        return DEVICE_ABSENT;
    }

    uint32_t status = 0;

    reg_write(port, VIRTIO_MMIO_STATUS, status);
    status |= VIRTIO_STATUS_ACKNOWLEDGE;
    reg_write(port, VIRTIO_MMIO_STATUS, status);
    status |= VIRTIO_STATUS_DRIVER;
    reg_write(port, VIRTIO_MMIO_STATUS, status);
    reg_write(port, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);

    if (!(reg_read(port, VIRTIO_MMIO_DEVICE_FEATURES) & VIRTIO_F_VERSION_1_HIGH_BIT))
    {
        return -1;
    }

    reg_write(port, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    reg_write(port, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    reg_write(port, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    reg_write(port, VIRTIO_MMIO_DRIVER_FEATURES, VIRTIO_F_VERSION_1_HIGH_BIT);
    status |= VIRTIO_STATUS_FEATURES_OK;
    reg_write(port, VIRTIO_MMIO_STATUS, status);

    if (!(reg_read(port, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK))
    {
        return -1;
    }

    reg_write(port, VIRTIO_MMIO_QUEUE_SEL, 0);

    if (reg_read(port, VIRTIO_MMIO_QUEUE_READY) != 0 || reg_read(port, VIRTIO_MMIO_QUEUE_NUM_MAX) < QUEUE_SIZE)
    {
        return -1;
    }

    void *d = page_alloc();
    void *a = page_alloc();
    void *u = page_alloc();
    void *e = page_alloc();

    if (!d || !a || !u || !e)
    {
        return -1;
    }

    zero(d);
    zero(a);
    zero(u);
    zero(e);
    port->desc = d;
    port->avail = a;
    port->used = u;
    port->events = e;
    port->used_seen = 0;

    for (uint16_t i = 0; i < QUEUE_SIZE; i++)
    {
        port->desc[i].addr = (uintptr_t)&port->events[i];
        port->desc[i].len = sizeof(input_raw_t);
        port->desc[i].flags = VIRTQ_DESC_F_WRITE;
        port->desc[i].next = 0;
        port->avail->ring[i] = i;
    }

    port->avail->idx = QUEUE_SIZE;
    reg_write(port, VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);
    set_address(port, VIRTIO_MMIO_QUEUE_DESC_LOW, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uintptr_t)d);
    set_address(port, VIRTIO_MMIO_QUEUE_DRIVER_LOW, VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uintptr_t)a);
    set_address(port, VIRTIO_MMIO_QUEUE_DEVICE_LOW, VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (uintptr_t)u);
    reg_write(port, VIRTIO_MMIO_QUEUE_READY, 1);
    status |= VIRTIO_STATUS_DRIVER_OK;
    reg_write(port, VIRTIO_MMIO_STATUS, status);
    __sync_synchronize();
    reg_write(port, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    return 0;
}

static void handle(const input_raw_t *event)
{
    switch (event->type)
    {
    case EV_KEY:
        if (event->code >= 0x110 && event->code < 0x118)
        {
            input_button(event->code, (int32_t)event->value);
        }
        else
        {
            input_key(event->code, (int32_t)event->value);
        }

        break;
    case EV_ABS:
        if (event->code <= 1)
        {
            input_absolute(event->code, (int32_t)event->value);
        }

        break;
    case EV_REL:
        if (event->code == REL_WHEEL)
        {
            input_wheel((int32_t)event->value);
        }

        break;
    case EV_SYN:
        input_sync();
        break;
    default:
        break;
    }
}

static void virtio_input_interrupt(device_t *dev)
{
    input_port_t *port = (input_port_t *)dev;

    reg_write(port, VIRTIO_MMIO_INTERRUPT_ACK, reg_read(port, VIRTIO_MMIO_INTERRUPT_STATUS) & 3);

    while (port->used_seen != port->used->idx)
    {
        __sync_synchronize();
        uint32_t id = port->used->ring[port->used_seen % QUEUE_SIZE].id % QUEUE_SIZE;

        handle(&port->events[id]);
        port->avail->ring[port->avail->idx % QUEUE_SIZE] = (uint16_t)id;
        __sync_synchronize();
        port->avail->idx = port->avail->idx + 1;
        port->used_seen++;
    }

    __sync_synchronize();
    reg_write(port, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
}

static input_port_t keyboard = {
    .device = {.name = "keyboard0", .irq = 5, .init = virtio_input_init, .interrupt = virtio_input_interrupt},
    .base = 0x10005000UL,
};

static input_port_t pointer = {
    .device = {.name = "mouse0", .irq = 6, .init = virtio_input_init, .interrupt = virtio_input_interrupt},
    .base = 0x10006000UL,
};

void virtio_input_register(void)
{
    device_register(&keyboard.device);
    device_register(&pointer.device);
}

#include "virtio_rng.h"
#include "virtio.h"
#include "mmio.h"
#include "device.h"
#include "page.h"
#include "spinlock.h"

#define RNG_BASE 0x10003000UL
#define RNG_IRQ 3
#define DEVICE_ID_RNG 4
#define QUEUE_SIZE 4
#define CHUNK 64
#define SPIN_LIMIT 10000000UL

typedef struct rng_avail
{
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QUEUE_SIZE];
    uint16_t used_event;
} rng_avail_t;

typedef struct rng_used
{
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[QUEUE_SIZE];
    uint16_t avail_event;
} rng_used_t;

static volatile virtq_desc_t *desc;
static volatile rng_avail_t *avail;
static volatile rng_used_t *used;
static uint16_t used_seen;
static uint8_t chunk[CHUNK] __attribute__((aligned(16)));
static int ready;
static device_t *self;

static uint32_t reg_read(uint32_t reg)
{
    return *(volatile uint32_t *)MMIO(RNG_BASE + reg);
}

static void reg_write(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)MMIO(RNG_BASE + reg) = value;
}

static void zero(void *memory, uint64_t length)
{
    uint8_t *p = memory;

    for (uint64_t i = 0; i < length; i++)
    {
        p[i] = 0;
    }
}

static void set_address(uint32_t low, uint32_t high, uintptr_t address)
{
    reg_write(low, (uint32_t)address);
    reg_write(high, (uint32_t)(address >> 32));
}

static int virtio_rng_init(device_t *dev)
{
    self = dev;

    if (reg_read(VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MAGIC || reg_read(VIRTIO_MMIO_VERSION) != VIRTIO_VERSION_MODERN ||
        reg_read(VIRTIO_MMIO_DEVICE_ID) != DEVICE_ID_RNG)
    {
        return -1;
    }

    uint32_t status = 0;

    reg_write(VIRTIO_MMIO_STATUS, status);
    status |= VIRTIO_STATUS_ACKNOWLEDGE;
    reg_write(VIRTIO_MMIO_STATUS, status);
    status |= VIRTIO_STATUS_DRIVER;
    reg_write(VIRTIO_MMIO_STATUS, status);

    reg_write(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);

    if (!(reg_read(VIRTIO_MMIO_DEVICE_FEATURES) & VIRTIO_F_VERSION_1_HIGH_BIT))
    {
        return -1;
    }

    reg_write(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    reg_write(VIRTIO_MMIO_DRIVER_FEATURES, 0);
    reg_write(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    reg_write(VIRTIO_MMIO_DRIVER_FEATURES, VIRTIO_F_VERSION_1_HIGH_BIT);
    status |= VIRTIO_STATUS_FEATURES_OK;
    reg_write(VIRTIO_MMIO_STATUS, status);

    if (!(reg_read(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK))
    {
        return -1;
    }

    reg_write(VIRTIO_MMIO_QUEUE_SEL, 0);

    if (reg_read(VIRTIO_MMIO_QUEUE_READY) != 0 || reg_read(VIRTIO_MMIO_QUEUE_NUM_MAX) < QUEUE_SIZE)
    {
        return -1;
    }

    void *d = page_alloc();
    void *a = page_alloc();
    void *u = page_alloc();

    if (!d || !a || !u)
    {
        return -1;
    }

    zero(d, PAGE_SIZE);
    zero(a, PAGE_SIZE);
    zero(u, PAGE_SIZE);
    desc = d;
    avail = a;
    used = u;
    used_seen = 0;
    reg_write(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);
    set_address(VIRTIO_MMIO_QUEUE_DESC_LOW, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uintptr_t)d);
    set_address(VIRTIO_MMIO_QUEUE_DRIVER_LOW, VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uintptr_t)a);
    set_address(VIRTIO_MMIO_QUEUE_DEVICE_LOW, VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (uintptr_t)u);
    reg_write(VIRTIO_MMIO_QUEUE_READY, 1);
    status |= VIRTIO_STATUS_DRIVER_OK;
    reg_write(VIRTIO_MMIO_STATUS, status);
    ready = 1;
    return 0;
}

static void virtio_rng_interrupt(device_t *dev)
{
    (void)dev;
    reg_write(VIRTIO_MMIO_INTERRUPT_ACK, reg_read(VIRTIO_MMIO_INTERRUPT_STATUS) & 3);
}

static int64_t fill_chunk(void)
{
    desc[0].addr = (uintptr_t)chunk;
    desc[0].len = CHUNK;
    desc[0].flags = VIRTQ_DESC_F_WRITE;
    desc[0].next = 0;
    avail->ring[avail->idx % QUEUE_SIZE] = 0;
    __sync_synchronize();
    avail->idx = avail->idx + 1;
    __sync_synchronize();
    reg_write(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    uint64_t spins = 0;

    while (used_seen == used->idx && spins++ < SPIN_LIMIT)
    {
        __sync_synchronize();
    }

    if (used_seen == used->idx)
    {
        return -1;
    }

    uint32_t length = used->ring[used_seen % QUEUE_SIZE].len;

    used_seen = used->idx;
    return length > CHUNK ? CHUNK : length;
}

int64_t virtio_rng_read(void *buffer, uint64_t length)
{
    static spinlock_t lock = SPINLOCK_INIT;

    if (!ready || (self && self->disabled))
    {
        return -1;
    }

    uint8_t *out = buffer;
    uint64_t done = 0;
    uint64_t interrupts = spin_lock(&lock);

    while (done < length)
    {
        int64_t got = fill_chunk();

        if (got <= 0)
        {
            break;
        }

        for (int64_t i = 0; i < got && done < length; i++)
        {
            out[done++] = chunk[i];
        }
    }

    spin_unlock(&lock, interrupts);
    return done == length ? (int64_t)done : -1;
}

static int64_t virtio_rng_device_read(device_t *dev, void *buffer, uint64_t length)
{
    (void)dev;
    return virtio_rng_read(buffer, length);
}

static device_t virtio_rng_device = {
    .name = "rng0",
    .irq = RNG_IRQ,
    .init = virtio_rng_init,
    .interrupt = virtio_rng_interrupt,
    .read = virtio_rng_device_read,
};

void virtio_rng_register(void)
{
    device_register(&virtio_rng_device);
}

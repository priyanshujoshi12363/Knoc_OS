#include "virtio_net.h"
#include "virtio.h"
#include "device.h"
#include "page.h"
#include "spinlock.h"
#include "net.h"

#define NET_BASE 0x10002000UL
#define NET_IRQ 2
#define DEVICE_ID_NET 1
#define FEATURE_MAC (1U << 5)
#define QUEUE_SIZE 16
#define RX_QUEUE 0
#define TX_QUEUE 1
#define HEADER_SIZE 12
#define BUFFER_SIZE 2048
#define TX_SPIN_LIMIT 10000000UL

typedef struct net_avail
{
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QUEUE_SIZE];
    uint16_t used_event;
} net_avail_t;

typedef struct net_used
{
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[QUEUE_SIZE];
    uint16_t avail_event;
} net_used_t;

typedef struct queue
{
    volatile virtq_desc_t *desc;
    volatile net_avail_t *avail;
    volatile net_used_t *used;
    uint16_t used_seen;
} queue_t;

static queue_t rx;
static queue_t tx;
static uint8_t rx_buffers[QUEUE_SIZE][BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buffer[BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t mac[6];
static int ready;

static uint32_t reg_read(uint32_t reg)
{
    return *(volatile uint32_t *)(NET_BASE + reg);
}

static void reg_write(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(NET_BASE + reg) = value;
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

static int setup_queue(queue_t *q, uint32_t index)
{
    reg_write(VIRTIO_MMIO_QUEUE_SEL, index);

    if (reg_read(VIRTIO_MMIO_QUEUE_READY) != 0 || reg_read(VIRTIO_MMIO_QUEUE_NUM_MAX) < QUEUE_SIZE)
    {
        return -1;
    }

    void *desc = page_alloc();
    void *avail = page_alloc();
    void *used = page_alloc();

    if (!desc || !avail || !used)
    {
        return -1;
    }

    zero(desc, PAGE_SIZE);
    zero(avail, PAGE_SIZE);
    zero(used, PAGE_SIZE);
    q->desc = desc;
    q->avail = avail;
    q->used = used;
    q->used_seen = 0;
    reg_write(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);
    set_address(VIRTIO_MMIO_QUEUE_DESC_LOW, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uintptr_t)desc);
    set_address(VIRTIO_MMIO_QUEUE_DRIVER_LOW, VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uintptr_t)avail);
    set_address(VIRTIO_MMIO_QUEUE_DEVICE_LOW, VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (uintptr_t)used);
    reg_write(VIRTIO_MMIO_QUEUE_READY, 1);
    return 0;
}

static void give_rx_buffer(uint16_t index)
{
    rx.desc[index].addr = (uintptr_t)rx_buffers[index];
    rx.desc[index].len = BUFFER_SIZE;
    rx.desc[index].flags = VIRTQ_DESC_F_WRITE;
    rx.desc[index].next = 0;
    rx.avail->ring[rx.avail->idx % QUEUE_SIZE] = index;
    __sync_synchronize();
    rx.avail->idx = rx.avail->idx + 1;
}

static int virtio_net_init(device_t *dev)
{
    (void)dev;

    if (reg_read(VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MAGIC || reg_read(VIRTIO_MMIO_VERSION) != VIRTIO_VERSION_MODERN ||
        reg_read(VIRTIO_MMIO_DEVICE_ID) != DEVICE_ID_NET)
    {
        return -1;
    }

    uint32_t status = 0;

    reg_write(VIRTIO_MMIO_STATUS, status);
    status |= VIRTIO_STATUS_ACKNOWLEDGE;
    reg_write(VIRTIO_MMIO_STATUS, status);
    status |= VIRTIO_STATUS_DRIVER;
    reg_write(VIRTIO_MMIO_STATUS, status);

    reg_write(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    uint32_t features_low = reg_read(VIRTIO_MMIO_DEVICE_FEATURES);
    reg_write(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);

    if (!(reg_read(VIRTIO_MMIO_DEVICE_FEATURES) & VIRTIO_F_VERSION_1_HIGH_BIT) || !(features_low & FEATURE_MAC))
    {
        return -1;
    }

    reg_write(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    reg_write(VIRTIO_MMIO_DRIVER_FEATURES, FEATURE_MAC);
    reg_write(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    reg_write(VIRTIO_MMIO_DRIVER_FEATURES, VIRTIO_F_VERSION_1_HIGH_BIT);
    status |= VIRTIO_STATUS_FEATURES_OK;
    reg_write(VIRTIO_MMIO_STATUS, status);

    if (!(reg_read(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK))
    {
        return -1;
    }

    if (setup_queue(&rx, RX_QUEUE) != 0 || setup_queue(&tx, TX_QUEUE) != 0)
    {
        return -1;
    }

    for (int i = 0; i < 6; i++)
    {
        mac[i] = *(volatile uint8_t *)(NET_BASE + VIRTIO_MMIO_CONFIG + i);
    }

    for (uint16_t i = 0; i < QUEUE_SIZE; i++)
    {
        give_rx_buffer(i);
    }

    status |= VIRTIO_STATUS_DRIVER_OK;
    reg_write(VIRTIO_MMIO_STATUS, status);
    reg_write(VIRTIO_MMIO_QUEUE_NOTIFY, RX_QUEUE);
    ready = 1;
    net_start(mac);
    return 0;
}

static void virtio_net_interrupt(device_t *dev)
{
    (void)dev;

    uint32_t status = reg_read(VIRTIO_MMIO_INTERRUPT_STATUS);

    reg_write(VIRTIO_MMIO_INTERRUPT_ACK, status & 3);
    __sync_synchronize();

    int given = 0;

    while (rx.used_seen != rx.used->idx)
    {
        volatile virtq_used_elem_t *element = &rx.used->ring[rx.used_seen % QUEUE_SIZE];
        uint16_t index = (uint16_t)element->id;
        uint32_t length = element->len;

        rx.used_seen++;

        if (index < QUEUE_SIZE && length > HEADER_SIZE)
        {
            net_receive(rx_buffers[index] + HEADER_SIZE, length - HEADER_SIZE);
        }

        if (index < QUEUE_SIZE)
        {
            give_rx_buffer(index);
            given = 1;
        }
    }

    if (given)
    {
        reg_write(VIRTIO_MMIO_QUEUE_NOTIFY, RX_QUEUE);
    }
}

int virtio_net_send(const void *frame, uint32_t length)
{
    static spinlock_t tx_lock = SPINLOCK_INIT;

    if (!ready || length + HEADER_SIZE > BUFFER_SIZE)
    {
        return -1;
    }

    uint64_t interrupts = spin_lock(&tx_lock);

    zero(tx_buffer, HEADER_SIZE);

    const uint8_t *source = frame;

    for (uint32_t i = 0; i < length; i++)
    {
        tx_buffer[HEADER_SIZE + i] = source[i];
    }

    tx.desc[0].addr = (uintptr_t)tx_buffer;
    tx.desc[0].len = HEADER_SIZE + length;
    tx.desc[0].flags = 0;
    tx.desc[0].next = 0;
    tx.avail->ring[tx.avail->idx % QUEUE_SIZE] = 0;
    __sync_synchronize();
    tx.avail->idx = tx.avail->idx + 1;
    __sync_synchronize();
    reg_write(VIRTIO_MMIO_QUEUE_NOTIFY, TX_QUEUE);

    uint64_t spins = 0;

    while (tx.used_seen == tx.used->idx && spins++ < TX_SPIN_LIMIT)
    {
        __sync_synchronize();
    }

    int result = tx.used_seen != tx.used->idx ? 0 : -1;

    tx.used_seen = tx.used->idx;
    spin_unlock(&tx_lock, interrupts);
    return result;
}

int virtio_net_ready(void)
{
    return ready;
}

static device_t virtio_net_device = {
    .name = "net0",
    .irq = NET_IRQ,
    .init = virtio_net_init,
    .interrupt = virtio_net_interrupt,
};

void virtio_net_register(void)
{
    device_register(&virtio_net_device);
}

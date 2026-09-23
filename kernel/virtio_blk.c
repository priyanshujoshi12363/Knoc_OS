#include "virtio_blk.h"
#include "process.h"
#include "spinlock.h"
#include "virtio.h"
#include "device.h"
#include "page.h"

#define VIRTIO_BLK_QUEUE_SIZE 8

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1

#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_STATUS_PENDING 0xFF

#define VIRTIO_INTERRUPT_MASK 0x3

typedef struct virtio_blk_request
{
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} virtio_blk_request_t;

typedef struct virtq_avail
{
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_BLK_QUEUE_SIZE];
    uint16_t used_event;
} virtq_avail_t;

typedef struct virtq_used
{
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[VIRTIO_BLK_QUEUE_SIZE];
    uint16_t avail_event;
} virtq_used_t;

static volatile virtq_desc_t *desc;
static volatile virtq_avail_t *avail;
static volatile virtq_used_t *used;

static uint16_t used_seen = 0;
static volatile int request_done = 0;

static virtio_blk_request_t request;
static uint8_t sector_buffer[VIRTIO_BLK_SECTOR_SIZE * VIRTIO_BLK_MAX_SECTORS]
    __attribute__((aligned(16)));

/* One request at a time: the descriptors and the buffer are shared */
static sleeplock_t disk_lock = SLEEPLOCK_INIT;
static char done_channel;
static volatile uint8_t request_status;

static uint32_t virtio_read(uint32_t reg)
{
    return *(volatile uint32_t *)(VIRTIO0_BASE + reg);
}

static void virtio_write(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(VIRTIO0_BASE + reg) = value;
}

static void clear_page(void *page)
{
    uint64_t *words = (uint64_t *)page;

    for (uint64_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++)
    {
        words[i] = 0;
    }
}

static void copy_bytes(void *destination, const void *source, uint64_t length)
{
    uint8_t *to = (uint8_t *)destination;
    const uint8_t *from = (const uint8_t *)source;

    for (uint64_t i = 0; i < length; i++)
    {
        to[i] = from[i];
    }
}

static void set_address(uint32_t low_reg, uint32_t high_reg, uintptr_t address)
{
    virtio_write(low_reg, (uint32_t)address);
    virtio_write(high_reg, (uint32_t)(address >> 32));
}

static int virtio_blk_init(device_t *dev)
{
    if (virtio_read(VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MAGIC ||
        virtio_read(VIRTIO_MMIO_VERSION) != VIRTIO_VERSION_MODERN ||
        virtio_read(VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_BLOCK)
    {
        return -1;
    }

    uint32_t status = 0;
    virtio_write(VIRTIO_MMIO_STATUS, status);

    status |= VIRTIO_STATUS_ACKNOWLEDGE;
    virtio_write(VIRTIO_MMIO_STATUS, status);

    status |= VIRTIO_STATUS_DRIVER;
    virtio_write(VIRTIO_MMIO_STATUS, status);

    virtio_write(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    uint32_t features_high = virtio_read(VIRTIO_MMIO_DEVICE_FEATURES);

    if (!(features_high & VIRTIO_F_VERSION_1_HIGH_BIT))
    {
        return -1;
    }

    virtio_write(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    virtio_write(VIRTIO_MMIO_DRIVER_FEATURES, 0);
    virtio_write(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    virtio_write(VIRTIO_MMIO_DRIVER_FEATURES, VIRTIO_F_VERSION_1_HIGH_BIT);

    status |= VIRTIO_STATUS_FEATURES_OK;
    virtio_write(VIRTIO_MMIO_STATUS, status);

    if (!(virtio_read(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK))
    {
        return -1;
    }

    virtio_write(VIRTIO_MMIO_QUEUE_SEL, 0);

    if (virtio_read(VIRTIO_MMIO_QUEUE_READY) != 0)
    {
        return -1;
    }

    if (virtio_read(VIRTIO_MMIO_QUEUE_NUM_MAX) < VIRTIO_BLK_QUEUE_SIZE)
    {
        return -1;
    }

    void *desc_page = page_alloc();
    void *avail_page = page_alloc();
    void *used_page = page_alloc();

    if (desc_page == 0 || avail_page == 0 || used_page == 0)
    {
        page_free(desc_page);
        page_free(avail_page);
        page_free(used_page);
        return -1;
    }

    clear_page(desc_page);
    clear_page(avail_page);
    clear_page(used_page);

    desc = (volatile virtq_desc_t *)desc_page;
    avail = (volatile virtq_avail_t *)avail_page;
    used = (volatile virtq_used_t *)used_page;

    virtio_write(VIRTIO_MMIO_QUEUE_NUM, VIRTIO_BLK_QUEUE_SIZE);

    set_address(VIRTIO_MMIO_QUEUE_DESC_LOW,
                VIRTIO_MMIO_QUEUE_DESC_HIGH,
                (uintptr_t)desc_page);

    set_address(VIRTIO_MMIO_QUEUE_DRIVER_LOW,
                VIRTIO_MMIO_QUEUE_DRIVER_HIGH,
                (uintptr_t)avail_page);

    set_address(VIRTIO_MMIO_QUEUE_DEVICE_LOW,
                VIRTIO_MMIO_QUEUE_DEVICE_HIGH,
                (uintptr_t)used_page);

    virtio_write(VIRTIO_MMIO_QUEUE_READY, 1);

    status |= VIRTIO_STATUS_DRIVER_OK;
    virtio_write(VIRTIO_MMIO_STATUS, status);

    uint64_t capacity_low = virtio_read(VIRTIO_MMIO_CONFIG);
    uint64_t capacity_high = virtio_read(VIRTIO_MMIO_CONFIG + 4);

    dev->block_size = VIRTIO_BLK_SECTOR_SIZE;
    dev->block_count = (capacity_high << 32) | capacity_low;

    return 0;
}

static void virtio_blk_interrupt(device_t *dev)
{
    (void)dev;

    uint32_t status = virtio_read(VIRTIO_MMIO_INTERRUPT_STATUS);
    virtio_write(VIRTIO_MMIO_INTERRUPT_ACK, status & VIRTIO_INTERRUPT_MASK);

    __sync_synchronize();

    while (used_seen != used->idx)
    {
        used_seen++;
        request_done = 1;
    }

    process_wake(&done_channel);
}

static void wait_done(void)
{
    uint64_t enabled = irq_save();

    while (!request_done)
    {
        if (process_can_block())
        {
            /* Other processes run while the disk works */
            process_block(&done_channel, 0);
        }
        else
        {
            irq_restore(enabled);
            asm volatile("wfi");
            enabled = irq_save();
        }
    }

    irq_restore(enabled);
}

static int virtio_blk_request(uint32_t type, uint64_t sector, uint64_t count)
{
    request.type = type;
    request.reserved = 0;
    request.sector = sector;
    request_status = VIRTIO_BLK_STATUS_PENDING;

    desc[0].addr = (uintptr_t)&request;
    desc[0].len = sizeof(request);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next = 1;

    desc[1].addr = (uintptr_t)sector_buffer;
    desc[1].len = VIRTIO_BLK_SECTOR_SIZE * count;
    desc[1].flags = VIRTQ_DESC_F_NEXT;

    if (type == VIRTIO_BLK_T_IN)
    {
        desc[1].flags |= VIRTQ_DESC_F_WRITE;
    }

    desc[1].next = 2;

    desc[2].addr = (uintptr_t)&request_status;
    desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next = 0;

    request_done = 0;

    avail->ring[avail->idx % VIRTIO_BLK_QUEUE_SIZE] = 0;

    __sync_synchronize();

    avail->idx = avail->idx + 1;

    __sync_synchronize();

    virtio_write(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    wait_done();

    __sync_synchronize();

    if (request_status != VIRTIO_BLK_S_OK)
    {
        return -1;
    }

    return 0;
}

static int virtio_blk_read_blocks(device_t *dev, uint64_t block, uint64_t count, void *buffer)
{
    if (count == 0 || count > VIRTIO_BLK_MAX_SECTORS || block + count > dev->block_count)
    {
        return -1;
    }

    sleeplock_acquire(&disk_lock);

    int result = virtio_blk_request(VIRTIO_BLK_T_IN, block, count);

    if (result == 0)
    {
        copy_bytes(buffer, sector_buffer, VIRTIO_BLK_SECTOR_SIZE * count);
    }

    sleeplock_release(&disk_lock);
    return result;
}

static int virtio_blk_write_blocks(device_t *dev, uint64_t block, uint64_t count, const void *buffer)
{
    if (count == 0 || count > VIRTIO_BLK_MAX_SECTORS || block + count > dev->block_count)
    {
        return -1;
    }

    sleeplock_acquire(&disk_lock);

    copy_bytes(sector_buffer, buffer, VIRTIO_BLK_SECTOR_SIZE * count);
    int result = virtio_blk_request(VIRTIO_BLK_T_OUT, block, count);

    sleeplock_release(&disk_lock);
    return result;
}

static int virtio_blk_read_block(device_t *dev, uint64_t block, void *buffer)
{
    return virtio_blk_read_blocks(dev, block, 1, buffer);
}

static int virtio_blk_write_block(device_t *dev, uint64_t block, const void *buffer)
{
    return virtio_blk_write_blocks(dev, block, 1, buffer);
}

static device_t virtio_blk_device = {
    .name = "disk0",
    .irq = VIRTIO0_IRQ,
    .init = virtio_blk_init,
    .interrupt = virtio_blk_interrupt,
    .read_block = virtio_blk_read_block,
    .write_block = virtio_blk_write_block,
    .read_blocks = virtio_blk_read_blocks,
    .write_blocks = virtio_blk_write_blocks,
};

void virtio_blk_register(void)
{
    device_register(&virtio_blk_device);
}

#include "virtio_gpu.h"
#include "virtio.h"
#include "mmio.h"
#include "device.h"
#include "page.h"
#include "spinlock.h"
#include "uart.h"

#define GPU_BASE 0x10004000UL
#define GPU_IRQ 4
#define DEVICE_ID_GPU 16
#define QUEUE_SIZE 8
#define SPIN_LIMIT 50000000UL

#define CMD_GET_DISPLAY_INFO 0x0100
#define CMD_RESOURCE_CREATE_2D 0x0101
#define CMD_SET_SCANOUT 0x0103
#define CMD_RESOURCE_FLUSH 0x0104
#define CMD_TRANSFER_TO_HOST_2D 0x0105
#define CMD_RESOURCE_ATTACH_BACKING 0x0106
#define CMD_UPDATE_CURSOR 0x0300
#define CMD_MOVE_CURSOR 0x0301
#define RESP_OK_NODATA 0x1100
#define RESP_OK_DISPLAY_INFO 0x1101

#define FORMAT_B8G8R8X8 2
#define FORMAT_B8G8R8A8 1
#define SCREEN_RESOURCE 1
#define CURSOR_RESOURCE 2
#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 800
#define MAX_SIDE 4096

typedef struct gpu_header
{
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} gpu_header_t;

typedef struct gpu_rect
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} gpu_rect_t;

typedef struct gpu_display_info
{
    gpu_header_t header;
    struct
    {
        gpu_rect_t rect;
        uint32_t enabled;
        uint32_t flags;
    } modes[16];
} gpu_display_info_t;

typedef struct gpu_create_2d
{
    gpu_header_t header;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} gpu_create_2d_t;

typedef struct gpu_attach_backing
{
    gpu_header_t header;
    uint32_t resource_id;
    uint32_t entries;
    uint64_t address;
    uint32_t length;
    uint32_t padding;
} gpu_attach_backing_t;

typedef struct gpu_set_scanout
{
    gpu_header_t header;
    gpu_rect_t rect;
    uint32_t scanout_id;
    uint32_t resource_id;
} gpu_set_scanout_t;

typedef struct gpu_transfer
{
    gpu_header_t header;
    gpu_rect_t rect;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} gpu_transfer_t;

typedef struct gpu_flush_cmd
{
    gpu_header_t header;
    gpu_rect_t rect;
    uint32_t resource_id;
    uint32_t padding;
} gpu_flush_cmd_t;

typedef struct gpu_cursor
{
    gpu_header_t header;
    uint32_t scanout_id;
    uint32_t x;
    uint32_t y;
    uint32_t padding0;
    uint32_t resource_id;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t padding1;
} gpu_cursor_t;

typedef struct gpu_avail
{
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QUEUE_SIZE];
    uint16_t used_event;
} gpu_avail_t;

typedef struct gpu_used
{
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[QUEUE_SIZE];
    uint16_t avail_event;
} gpu_used_t;

typedef struct gpu_queue
{
    volatile virtq_desc_t *desc;
    volatile gpu_avail_t *avail;
    volatile gpu_used_t *used;
    uint16_t used_seen;
} gpu_queue_t;

static gpu_queue_t control;
static gpu_queue_t cursorq;
static uint8_t *request;
static uint8_t *response;
static uint8_t *cursor_request;
static uint32_t *framebuffer;
static uint32_t *cursor_pixels;
static uint32_t width;
static uint32_t height;
static int ready;
static int cursor_ready;
static device_t *self;
static spinlock_t lock = SPINLOCK_INIT;

static uint32_t reg_read(uint32_t reg)
{
    return *(volatile uint32_t *)MMIO(GPU_BASE + reg);
}

static void reg_write(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)MMIO(GPU_BASE + reg) = value;
}

static void zero(void *memory, uint64_t length)
{
    uint64_t *words = memory;

    for (uint64_t i = 0; i < length / 8; i++)
    {
        words[i] = 0;
    }
}

static void set_address(uint32_t low, uint32_t high, uintptr_t address)
{
    reg_write(low, (uint32_t)address);
    reg_write(high, (uint32_t)(address >> 32));
}

static int queue_setup(gpu_queue_t *queue, uint32_t index)
{
    reg_write(VIRTIO_MMIO_QUEUE_SEL, index);

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
    queue->desc = d;
    queue->avail = a;
    queue->used = u;
    queue->used_seen = 0;
    queue->avail->flags = 1;
    reg_write(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);
    set_address(VIRTIO_MMIO_QUEUE_DESC_LOW, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uintptr_t)d);
    set_address(VIRTIO_MMIO_QUEUE_DRIVER_LOW, VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uintptr_t)a);
    set_address(VIRTIO_MMIO_QUEUE_DEVICE_LOW, VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (uintptr_t)u);
    reg_write(VIRTIO_MMIO_QUEUE_READY, 1);
    return 0;
}

static int submit(gpu_queue_t *queue, uint32_t index, void *out, uint32_t out_length, void *in, uint32_t in_length)
{
    queue->desc[0].addr = (uintptr_t)out;
    queue->desc[0].len = out_length;
    queue->desc[0].flags = in ? VIRTQ_DESC_F_NEXT : 0;
    queue->desc[0].next = 1;

    if (in)
    {
        queue->desc[1].addr = (uintptr_t)in;
        queue->desc[1].len = in_length;
        queue->desc[1].flags = VIRTQ_DESC_F_WRITE;
        queue->desc[1].next = 0;
    }

    queue->avail->ring[queue->avail->idx % QUEUE_SIZE] = 0;
    __sync_synchronize();
    queue->avail->idx = queue->avail->idx + 1;
    __sync_synchronize();
    reg_write(VIRTIO_MMIO_QUEUE_NOTIFY, index);

    uint64_t spins = 0;

    while (queue->used_seen == queue->used->idx && spins++ < SPIN_LIMIT)
    {
        __sync_synchronize();
    }

    if (queue->used_seen == queue->used->idx)
    {
        return -1;
    }

    queue->used_seen = queue->used->idx;
    return 0;
}

static gpu_header_t *command(uint32_t type, uint32_t length)
{
    gpu_header_t *header = (gpu_header_t *)request;

    zero(request, (length + 7) & ~7U);
    header->type = type;
    return header;
}

static int send(uint32_t length, uint32_t response_length, uint32_t expected)
{
    zero(response, (response_length + 7) & ~7U);

    if (submit(&control, 0, request, length, response, response_length) != 0)
    {
        return -1;
    }

    return ((gpu_header_t *)response)->type == expected ? 0 : -1;
}

static int create_resource(uint32_t id, uint32_t format, uint32_t w, uint32_t h, void *backing, uint64_t bytes)
{
    gpu_create_2d_t *create = (gpu_create_2d_t *)command(CMD_RESOURCE_CREATE_2D, sizeof(gpu_create_2d_t));

    create->resource_id = id;
    create->format = format;
    create->width = w;
    create->height = h;

    if (send(sizeof(gpu_create_2d_t), sizeof(gpu_header_t), RESP_OK_NODATA) != 0)
    {
        return -1;
    }

    gpu_attach_backing_t *attach =
        (gpu_attach_backing_t *)command(CMD_RESOURCE_ATTACH_BACKING, sizeof(gpu_attach_backing_t));

    attach->resource_id = id;
    attach->entries = 1;
    attach->address = (uintptr_t)backing;
    attach->length = (uint32_t)bytes;
    return send(sizeof(gpu_attach_backing_t), sizeof(gpu_header_t), RESP_OK_NODATA);
}

static int transfer(uint32_t id, uint32_t stride, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    gpu_transfer_t *t = (gpu_transfer_t *)command(CMD_TRANSFER_TO_HOST_2D, sizeof(gpu_transfer_t));

    t->rect.x = x;
    t->rect.y = y;
    t->rect.width = w;
    t->rect.height = h;
    t->offset = (uint64_t)y * stride + (uint64_t)x * 4;
    t->resource_id = id;
    return send(sizeof(gpu_transfer_t), sizeof(gpu_header_t), RESP_OK_NODATA);
}

static int virtio_gpu_init(device_t *dev)
{
    self = dev;

    if (reg_read(VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MAGIC || reg_read(VIRTIO_MMIO_VERSION) != VIRTIO_VERSION_MODERN ||
        reg_read(VIRTIO_MMIO_DEVICE_ID) != DEVICE_ID_GPU)
    {
        return DEVICE_ABSENT;
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

    if (queue_setup(&control, 0) != 0 || queue_setup(&cursorq, 1) != 0)
    {
        return -1;
    }

    request = page_alloc();
    response = page_alloc();
    cursor_request = page_alloc();

    if (!request || !response || !cursor_request)
    {
        return -1;
    }

    status |= VIRTIO_STATUS_DRIVER_OK;
    reg_write(VIRTIO_MMIO_STATUS, status);

    width = DEFAULT_WIDTH;
    height = DEFAULT_HEIGHT;
    command(CMD_GET_DISPLAY_INFO, sizeof(gpu_header_t));

    if (send(sizeof(gpu_header_t), sizeof(gpu_display_info_t), RESP_OK_DISPLAY_INFO) == 0)
    {
        gpu_display_info_t *info = (gpu_display_info_t *)response;

        if (info->modes[0].enabled && info->modes[0].rect.width >= 320 && info->modes[0].rect.height >= 200 &&
            info->modes[0].rect.width <= MAX_SIDE && info->modes[0].rect.height <= MAX_SIDE)
        {
            width = info->modes[0].rect.width & ~7U;
            height = info->modes[0].rect.height;
        }
    }

    uint64_t bytes = (uint64_t)width * height * 4;

    framebuffer = page_alloc_contiguous(bytes);

    if (!framebuffer)
    {
        return -1;
    }

    zero(framebuffer, bytes);

    if (create_resource(SCREEN_RESOURCE, FORMAT_B8G8R8X8, width, height, framebuffer, bytes) != 0)
    {
        return -1;
    }

    gpu_set_scanout_t *scanout = (gpu_set_scanout_t *)command(CMD_SET_SCANOUT, sizeof(gpu_set_scanout_t));

    scanout->rect.width = width;
    scanout->rect.height = height;
    scanout->scanout_id = 0;
    scanout->resource_id = SCREEN_RESOURCE;

    if (send(sizeof(gpu_set_scanout_t), sizeof(gpu_header_t), RESP_OK_NODATA) != 0)
    {
        return -1;
    }

    cursor_pixels = page_alloc_contiguous(GPU_CURSOR_SIZE * GPU_CURSOR_SIZE * 4);

    if (cursor_pixels)
    {
        zero(cursor_pixels, GPU_CURSOR_SIZE * GPU_CURSOR_SIZE * 4);
        cursor_ready = create_resource(CURSOR_RESOURCE, FORMAT_B8G8R8A8, GPU_CURSOR_SIZE, GPU_CURSOR_SIZE,
                                       cursor_pixels, GPU_CURSOR_SIZE * GPU_CURSOR_SIZE * 4) == 0;
    }

    ready = 1;
    uart_puts("[INFO] Screen: ");
    uart_put_uint(width);
    uart_putc('x');
    uart_put_uint(height);
    uart_puts(" pixels, 32-bit colour (virtio-gpu)\n");
    return 0;
}

static void virtio_gpu_interrupt(device_t *dev)
{
    (void)dev;
    reg_write(VIRTIO_MMIO_INTERRUPT_ACK, reg_read(VIRTIO_MMIO_INTERRUPT_STATUS) & 3);
}

int gpu_ready(void)
{
    return ready && !(self && self->disabled);
}

uint32_t *gpu_framebuffer(void)
{
    return framebuffer;
}

uint32_t gpu_width(void)
{
    return width;
}

uint32_t gpu_height(void)
{
    return height;
}

uint64_t gpu_framebuffer_bytes(void)
{
    return (uint64_t)width * height * 4;
}

int gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!gpu_ready() || x >= width || y >= height || w == 0 || h == 0)
    {
        return -1;
    }

    if (w > width - x)
    {
        w = width - x;
    }

    if (h > height - y)
    {
        h = height - y;
    }

    uint64_t interrupts = spin_lock(&lock);
    int result = transfer(SCREEN_RESOURCE, width * 4, x, y, w, h);

    if (result == 0)
    {
        gpu_flush_cmd_t *flush = (gpu_flush_cmd_t *)command(CMD_RESOURCE_FLUSH, sizeof(gpu_flush_cmd_t));

        flush->rect.x = x;
        flush->rect.y = y;
        flush->rect.width = w;
        flush->rect.height = h;
        flush->resource_id = SCREEN_RESOURCE;
        result = send(sizeof(gpu_flush_cmd_t), sizeof(gpu_header_t), RESP_OK_NODATA);
    }

    spin_unlock(&lock, interrupts);
    return result;
}

static int cursor_command(uint32_t type, uint32_t resource, uint32_t x, uint32_t y, uint32_t hot_x, uint32_t hot_y)
{
    gpu_cursor_t *cursor = (gpu_cursor_t *)cursor_request;

    zero(cursor, sizeof(gpu_cursor_t));
    cursor->header.type = type;
    cursor->x = x;
    cursor->y = y;
    cursor->resource_id = resource;
    cursor->hot_x = hot_x;
    cursor->hot_y = hot_y;
    return submit(&cursorq, 1, cursor, sizeof(gpu_cursor_t), 0, 0);
}

int gpu_cursor_set(const uint32_t *pixels, uint32_t hot_x, uint32_t hot_y)
{
    if (!gpu_ready() || !cursor_ready)
    {
        return -1;
    }

    uint64_t interrupts = spin_lock(&lock);

    for (uint32_t i = 0; i < GPU_CURSOR_SIZE * GPU_CURSOR_SIZE; i++)
    {
        cursor_pixels[i] = pixels[i];
    }

    int result = transfer(CURSOR_RESOURCE, GPU_CURSOR_SIZE * 4, 0, 0, GPU_CURSOR_SIZE, GPU_CURSOR_SIZE);

    if (result == 0)
    {
        result = cursor_command(CMD_UPDATE_CURSOR, CURSOR_RESOURCE, width / 2, height / 2, hot_x, hot_y);
    }

    spin_unlock(&lock, interrupts);
    return result;
}

int gpu_cursor_move(uint32_t x, uint32_t y)
{
    if (!gpu_ready() || !cursor_ready)
    {
        return -1;
    }

    uint64_t interrupts = spin_lock(&lock);
    int result = cursor_command(CMD_MOVE_CURSOR, CURSOR_RESOURCE, x, y, 0, 0);

    spin_unlock(&lock, interrupts);
    return result;
}

int gpu_cursor_hide(void)
{
    if (!gpu_ready() || !cursor_ready)
    {
        return -1;
    }

    uint64_t interrupts = spin_lock(&lock);
    int result = cursor_command(CMD_UPDATE_CURSOR, 0, 0, 0, 0, 0);

    spin_unlock(&lock, interrupts);
    return result;
}

static device_t virtio_gpu_device = {
    .name = "gpu0",
    .irq = GPU_IRQ,
    .init = virtio_gpu_init,
    .interrupt = virtio_gpu_interrupt,
};

void virtio_gpu_register(void)
{
    device_register(&virtio_gpu_device);
}

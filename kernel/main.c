#include <stdint.h>
#include "logging.h"
#include "page.h"
#include "vm.h"
#include "heap.h"
#include "timer.h"
#include "trap.h"
#include "uart.h"
#include "plic.h"
#include "power.h"
#include "device.h"
#include "virtio_blk.h"

#define TIMER_TEST_TICKS 5
#define KEY_CTRL_D 0x04
#define KEY_BACKSPACE 0x7F

#define DISK_HOST_SECTOR 0
#define DISK_BOOT_COUNT_SECTOR 1
#define DISK_TEST_SECTOR 2
#define DISK_BOOT_COUNT_MAGIC 0x544F4F42434F4E4BULL
#define DISK_TEXT_MAX 64

typedef struct disk_boot_record
{
    uint64_t magic;
    uint64_t count;
} disk_boot_record_t;

static uint8_t disk_buffer[VIRTIO_BLK_SECTOR_SIZE];

static void disk_self_test(device_t *disk)
{
    log_info_uint("Disk sectors: ", disk->block_count);

    for (uint64_t i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++)
    {
        disk_buffer[i] = (uint8_t)(i * 7 + 3);
    }

    if (device_write_block(disk, DISK_TEST_SECTOR, disk_buffer) != 0)
    {
        panic("Disk write failed");
    }

    for (uint64_t i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++)
    {
        disk_buffer[i] = 0;
    }

    if (device_read_block(disk, DISK_TEST_SECTOR, disk_buffer) != 0)
    {
        panic("Disk read failed");
    }

    for (uint64_t i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++)
    {
        if (disk_buffer[i] != (uint8_t)(i * 7 + 3))
        {
            panic("Disk data mismatch");
        }
    }

    log_info("Disk write/read test passed");

    if (device_read_block(disk, DISK_HOST_SECTOR, disk_buffer) != 0)
    {
        panic("Disk read failed");
    }

    char text[DISK_TEXT_MAX];
    uint64_t length = 0;

    while (length < DISK_TEXT_MAX - 1 &&
           disk_buffer[length] >= ' ' &&
           disk_buffer[length] <= '~')
    {
        text[length] = (char)disk_buffer[length];
        length++;
    }

    text[length] = 0;
    log_info_text("Disk says: ", text);

    if (device_read_block(disk, DISK_BOOT_COUNT_SECTOR, disk_buffer) != 0)
    {
        panic("Disk read failed");
    }

    disk_boot_record_t *record = (disk_boot_record_t *)disk_buffer;

    if (record->magic != DISK_BOOT_COUNT_MAGIC)
    {
        record->magic = DISK_BOOT_COUNT_MAGIC;
        record->count = 0;
    }

    record->count++;

    if (device_write_block(disk, DISK_BOOT_COUNT_SECTOR, disk_buffer) != 0)
    {
        panic("Disk write failed");
    }

    log_info_uint("Disk boot count: ", record->count);
}

void kernel_main(void)
{
    log_info("KnocOS " KNOCOS_VERSION " starting");

    trap_enable_interrupts();
    log_info("Supervisor interrupts enabled");

    page_init();
    log_info("Page memory initialized");

    vm_init();
    log_info("Virtual memory initialized");

    log_info("Kernel page tables ready");

    heap_init();
    log_info("Kernel heap mapping prepared");

    unsigned long pages_before =
        page_used();

    log_info("Enabling Sv39");

    vm_enable();

    log_info("Sv39 enabled");

    heap_activate();

    log_info("Kernel heap activated");

    void *block_a = kmalloc(3000);

    if (block_a == 0)
    {
        panic("Allocation A failed");
    }

    log_info("Allocation A successful");

    void *block_b = kmalloc(3000);

    if (block_b == 0)
    {
        panic("Allocation B failed");
    }

    log_info("Allocation B successful");

    void *block_c = kmalloc(3000);

    if (block_c == 0)
    {
        panic("Allocation C failed");
    }

    log_info("Allocation C successful");

    void *block_d = kmalloc(8000);

    if (block_d == 0)
    {
        panic("Allocation D failed");
    }

    log_info("Allocation D successful");

    void *block_e = kmalloc(16000);

    if (block_e == 0)
    {
        panic("Allocation E failed");
    }

    log_info("Allocation E successful");

    volatile uint64_t *value_a =
        (volatile uint64_t *)block_a;

    volatile uint64_t *value_b =
        (volatile uint64_t *)block_b;

    volatile uint64_t *value_c =
        (volatile uint64_t *)block_c;

    volatile uint64_t *value_d =
        (volatile uint64_t *)block_d;

    volatile uint64_t *value_e =
        (volatile uint64_t *)block_e;

    *value_a = 0xAAAAAAAAAAAAAAAAULL;
    *value_b = 0xBBBBBBBBBBBBBBBBULL;
    *value_c = 0xCCCCCCCCCCCCCCCCULL;
    *value_d = 0xDDDDDDDDDDDDDDDDULL;
    *value_e = 0xEEEEEEEEEEEEEEEEULL;

    if (*value_a != 0xAAAAAAAAAAAAAAAAULL)
    {
        panic("Allocation A memory test failed");
    }

    if (*value_b != 0xBBBBBBBBBBBBBBBBULL)
    {
        panic("Allocation B memory test failed");
    }

    if (*value_c != 0xCCCCCCCCCCCCCCCCULL)
    {
        panic("Allocation C memory test failed");
    }

    if (*value_d != 0xDDDDDDDDDDDDDDDDULL)
    {
        panic("Allocation D memory test failed");
    }

    if (*value_e != 0xEEEEEEEEEEEEEEEEULL)
    {
        panic("Allocation E memory test failed");
    }

    log_info("Multiple heap growth memory test passed");

    unsigned long pages_after =
        page_used();

    if (pages_after <= pages_before + 1)
    {
        panic("Heap did not grow multiple pages");
    }

    log_info("Multiple physical page growth verified");

    kfree(block_a);
    kfree(block_b);
    kfree(block_c);
    kfree(block_d);
    kfree(block_e);

    log_info("All heap blocks freed");

    void *block_f = kmalloc(20000);

    if (block_f == 0)
    {
        panic("Post-free allocation failed");
    }

    log_info("Post-free heap allocation successful");

    volatile uint64_t *value_f =
        (volatile uint64_t *)block_f;

    *value_f = 0xFFFFFFFFFFFFFFFFULL;

    if (*value_f != 0xFFFFFFFFFFFFFFFFULL)
    {
        panic("Post-free memory test failed");
    }

    log_info("Heap reuse and growth verified");

    kfree(block_f);

    log_info("Kernel heap 4.0 stress test passed");

    uint64_t timer_start = timer_ticks();
    uint64_t time_start = timer_read();

    while (timer_ticks() < timer_start + TIMER_TEST_TICKS)
    {
        if (timer_read() - time_start >= TIMER_FREQ_HZ)
        {
            panic("Timer interrupt timeout");
        }
    }

    if (timer_read() - time_start <
        (TIMER_TEST_TICKS - 1) * TIMER_INTERVAL)
    {
        panic("Timer ticks arrived too fast");
    }

    log_info("Supervisor timer interrupts verified");

    uint64_t breakpoints_before = trap_breakpoint_count();

    asm volatile("ebreak");

    if (trap_breakpoint_count() != breakpoints_before + 1)
    {
        panic("Supervisor trap test failed");
    }

    log_info("Supervisor trap handler verified");

    plic_init();

    uart_register();
    power_register();
    virtio_blk_register();

    device_init_all();
    device_list();

    device_t *console = device_find("uart0");
    device_t *power = device_find("power0");

    if (console == 0 || power == 0)
    {
        panic("Required device missing");
    }

    if (device_count() != 3 || device_find("missing0") != 0)
    {
        panic("Device table test failed");
    }

    log_info("Device table verified");

    device_t *disk = device_find("disk0");

    if (disk != 0 && disk->ready)
    {
        disk_self_test(disk);
    }
    else
    {
        log_warn("No disk attached");
    }

    log_info("All self-tests passed");
    log_info("Keyboard echo ready, start typing (Ctrl-D to power off)");

    while (1)
    {
        char c;

        if (device_read(console, &c, 1) <= 0)
        {
            asm volatile("wfi");
            continue;
        }

        if (c == KEY_CTRL_D)
        {
            uint8_t command = POWER_COMMAND_OFF;

            device_write(console, "\n", 1);
            log_info("Powering off");
            device_write(power, &command, 1);
        }
        else if (c == '\r')
        {
            device_write(console, "\n", 1);
        }
        else if (c == KEY_BACKSPACE)
        {
            device_write(console, "\b \b", 3);
        }
        else
        {
            device_write(console, &c, 1);
        }
    }
}

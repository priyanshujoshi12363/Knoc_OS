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
#include "process.h"

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

#define SCHED_TEST_TICKS 100
#define SCHED_TEST_WORKERS 3
#define SCHED_TEST_TOLERANCE 10

typedef struct sched_worker
{
    const char *name;
    process_class_t process_class;
    uint64_t expected_percent;
    int pid;
    volatile uint64_t work;
} sched_worker_t;

static uint8_t disk_buffer[VIRTIO_BLK_SECTOR_SIZE];

static device_t *console;
static device_t *power;

static sched_worker_t sched_workers[SCHED_TEST_WORKERS] = {
    {"agent-coder", PROCESS_CLASS_AI_AGENT, SCHED_WEIGHT_AI_AGENT, 0, 0},
    {"normal-task", PROCESS_CLASS_NORMAL, SCHED_WEIGHT_NORMAL, 0, 0},
    {"nn-sorter", PROCESS_CLASS_BACKGROUND, SCHED_WEIGHT_BACKGROUND, 0, 0},
};

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

static void console_process(void *arg)
{
    (void)arg;

    log_info("Keyboard echo ready, start typing (Ctrl-D to power off)");

    while (1)
    {
        char c;

        if (device_read(console, &c, 1) <= 0)
        {
            process_sleep(1);
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

static void cpu_worker(void *arg)
{
    sched_worker_t *worker = (sched_worker_t *)arg;

    while (1)
    {
        worker->work++;
    }
}

static void scheduler_test(void *arg)
{
    (void)arg;

    for (int i = 0; i < SCHED_TEST_WORKERS; i++)
    {
        sched_worker_t *worker = &sched_workers[i];

        worker->pid = process_create(worker->name,
                                     worker->process_class,
                                     cpu_worker,
                                     worker);

        if (worker->pid < 0)
        {
            panic("Could not create worker process");
        }
    }

    log_info("Workers created: agent-coder (AI_AGENT), normal-task (NORMAL), nn-sorter (BACKGROUND)");

    uint64_t wake_target = timer_ticks() + SCHED_TEST_TICKS;

    process_sleep(SCHED_TEST_TICKS);

    uint64_t wake_latency = timer_ticks() - wake_target;

    process_list();

    uint64_t total = 0;

    for (int i = 0; i < SCHED_TEST_WORKERS; i++)
    {
        total += process_cpu_ticks(sched_workers[i].pid);
    }

    if (total == 0)
    {
        panic("Workers did not run");
    }

    for (int i = 0; i < SCHED_TEST_WORKERS; i++)
    {
        sched_worker_t *worker = &sched_workers[i];
        uint64_t percent = process_cpu_ticks(worker->pid) * 100 / total;

        uart_puts("[INFO] CPU share: ");
        uart_puts(worker->name);
        uart_puts(" ");
        uart_put_uint(percent);
        uart_puts("% (expected ");
        uart_put_uint(worker->expected_percent);
        uart_puts("%)\n");

        if (percent + SCHED_TEST_TOLERANCE < worker->expected_percent ||
            percent > worker->expected_percent + SCHED_TEST_TOLERANCE)
        {
            panic("CPU share outside expected range");
        }

        if (worker->work == 0)
        {
            panic("A worker made no progress");
        }
    }

    log_info("AI-aware scheduling verified");
    log_info("Preemption verified: CPU-bound workers never yield, all made progress");

    if (wake_latency > 1)
    {
        panic("Interactive process woke too late");
    }

    log_info_uint("Interactive wake latency (ticks): ", wake_latency);
    log_info("Interactive response verified");

    for (int i = 0; i < SCHED_TEST_WORKERS; i++)
    {
        process_kill(sched_workers[i].pid);
    }

    log_info("All self-tests passed");

    if (process_create("console", PROCESS_CLASS_INTERACTIVE, console_process, 0) < 0)
    {
        panic("Could not create console process");
    }
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

    console = device_find("uart0");
    power = device_find("power0");

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

    process_init();

    if (process_create("sched-test", PROCESS_CLASS_INTERACTIVE, scheduler_test, 0) < 0)
    {
        panic("Could not create scheduler test process");
    }

    scheduler_start();

    while (1)
    {
        asm volatile("wfi");
    }
}

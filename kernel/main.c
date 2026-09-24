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
#include "guardian.h"
#include "aispace.h"
#include "blackbox.h"
#include "faulty.h"
#include "fdt.h"
#include "spinlock.h"
#include "program.h"
#include "knocfs.h"
#include "tty.h"
#include "memgraph.h"
#include "telemetry.h"

#define TIMER_TEST_TICKS 5
#define KEY_CTRL_C 0x03
#define KEY_CTRL_D 0x04
#define KEY_CTRL_E 0x05
#define KEY_CTRL_U 0x15
#define KEY_BACKSPACE 0x7F
#define KEY_CTRL_F 0x06
#define KEY_CTRL_K 0x0B
#define KEY_CTRL_O 0x0F
#define KEY_CTRL_P 0x10
#define KEY_CTRL_W 0x17
#define KEY_CTRL_X 0x18

#define TEST_FAULT_ADDRESS 0x40000000UL

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

#define MEMTEST_BLOCK_BYTES (64UL * 1024 * 1024)
#define MEMTEST_PAGES 1024
#define BYTES_PER_MIB (1024UL * 1024)

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
static device_t *faulty;

static sched_worker_t sched_workers[SCHED_TEST_WORKERS] = {
    {"agent-coder", PROCESS_CLASS_AI_AGENT, SCHED_WEIGHT_AI_AGENT, 0, 0},
    {"normal-task", PROCESS_CLASS_NORMAL, SCHED_WEIGHT_NORMAL, 0, 0},
    {"nn-sorter", PROCESS_CLASS_BACKGROUND, SCHED_WEIGHT_BACKGROUND, 0, 0},
};

static uint32_t boot_number;
static int health_started;

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
    boot_number = (uint32_t)record->count;
}

static void print_mib(const char *label, uint64_t bytes)
{
    uart_puts(label);
    uart_put_uint(bytes / BYTES_PER_MIB);
    uart_puts(" MiB");
}

static void spinlock_self_test(void)
{
    spinlock_t lock = SPINLOCK_INIT;
    uint64_t interrupts = spin_lock(&lock);
    uint64_t sstatus;

    asm volatile("csrr %0, sstatus" : "=r"(sstatus));

    if (sstatus & SSTATUS_SIE)
    {
        panic("Spinlock left interrupts on");
    }

    if (spin_trylock(&lock))
    {
        panic("Spinlock taken twice");
    }

    spin_unlock(&lock, interrupts);

    if (!spin_trylock(&lock))
    {
        panic("Spinlock not released");
    }

    log_info("Spinlock verified: exclusive, interrupts off while held");
}

static void check_block(uint8_t *block, uint64_t bytes, uint64_t step)
{
    for (uint64_t offset = 0; offset < bytes; offset += step)
    {
        *(volatile uint64_t *)(block + offset) = (uintptr_t)(block + offset) ^ 0x5A5A5A5A5A5A5A5AULL;
    }

    *(volatile uint64_t *)(block + bytes - 8) = 0x1234567812345678ULL;

    for (uint64_t offset = 0; offset < bytes; offset += step)
    {
        if (*(volatile uint64_t *)(block + offset) != ((uintptr_t)(block + offset) ^ 0x5A5A5A5A5A5A5A5AULL))
        {
            panic("Memory block read back wrong data");
        }
    }

    if (*(volatile uint64_t *)(block + bytes - 8) != 0x1234567812345678ULL)
    {
        panic("Memory block end read back wrong data");
    }
}

static void memory_self_test(void)
{
    void **pages = kmalloc(MEMTEST_PAGES * sizeof(void *));

    if (pages == 0)
    {
        panic("Memory test allocation failed");
    }

    unsigned long free_before = page_free_count();
    unsigned long largest_before = page_largest_free();

    uint8_t *block = page_alloc_contiguous(MEMTEST_BLOCK_BYTES);

    if (block == 0 || (uintptr_t)block % MEMTEST_BLOCK_BYTES != 0)
    {
        panic("No aligned 64 MiB contiguous block");
    }

    check_block(block, MEMTEST_BLOCK_BYTES, VM_MEGAPAGE_SIZE);
    page_free(block);

    for (int i = 0; i < MEMTEST_PAGES; i++)
    {
        pages[i] = page_alloc();

        if (pages[i] == 0)
        {
            panic("Single page allocation failed");
        }
    }

    for (int i = 0; i < MEMTEST_PAGES; i += 2)
    {
        page_free(pages[i]);
    }

    for (int i = 1; i < MEMTEST_PAGES; i += 2)
    {
        page_free(pages[i]);
    }

    if (page_free_count() != free_before || page_largest_free() != largest_before)
    {
        panic("Buddy blocks did not merge back");
    }

    kfree(pages);

    log_info("Buddy allocator verified: 64 MiB contiguous block, 1024 single pages freed and merged back");

    uint64_t largest_bytes = largest_before * PAGE_SIZE;
    uint8_t *large = page_alloc_contiguous(largest_bytes);

    if (large == 0)
    {
        panic("Largest free block allocation failed");
    }

    check_block(large, largest_bytes, VM_MEGAPAGE_SIZE);
    page_free(large);

    print_mib("[INFO] Large memory verified: ", largest_bytes);
    uart_puts(" block at ");
    uart_put_hex((uintptr_t)large);
    uart_puts(" read and written through megapages\n");
}

#define WAIT_TEST_READS 200

static volatile uint64_t wait_test_work;
static uint8_t wait_test_single[VIRTIO_BLK_SECTOR_SIZE * VIRTIO_BLK_MAX_SECTORS];
static uint8_t wait_test_multi[VIRTIO_BLK_SECTOR_SIZE * VIRTIO_BLK_MAX_SECTORS];

static void wait_test_worker(void *arg)
{
    (void)arg;

    while (1)
    {
        wait_test_work++;
    }
}

/* While this process waits for the disk it must sleep, so another
   process gets the CPU even between timer ticks */
static void wait_queue_test(void)
{
    device_t *disk = device_find("disk0");

    if (disk == 0 || !disk->ready)
    {
        log_warn("Wait queue test skipped: no disk");
        return;
    }

    int worker = process_create("wait-worker", PROCESS_CLASS_NORMAL, wait_test_worker, 0);

    if (worker < 0)
    {
        panic("Could not create the wait test worker");
    }

    uint64_t blocks_before = process_block_count();
    uint64_t work_before = wait_test_work;

    for (int i = 0; i < WAIT_TEST_READS; i++)
    {
        if (device_read_block(disk, (uint64_t)(i % 16), wait_test_single) != 0)
        {
            panic("Disk read failed");
        }
    }

    uint64_t blocks = process_block_count() - blocks_before;
    uint64_t work = wait_test_work - work_before;

    process_kill(worker);

    /* A read that finished before the driver checked needs no sleep, but
       every read that did sleep must have let the worker run */
    if (blocks > 0 && work == 0)
    {
        panic("Disk reads slept but no other process ran");
    }

    for (int i = 0; i < VIRTIO_BLK_MAX_SECTORS; i++)
    {
        if (device_read_block(disk, (uint64_t)i, wait_test_single + i * VIRTIO_BLK_SECTOR_SIZE) != 0)
        {
            panic("Disk read failed");
        }
    }

    if (device_read_blocks(disk, 0, VIRTIO_BLK_MAX_SECTORS, wait_test_multi) != 0)
    {
        panic("Multi-sector read failed");
    }

    for (uint64_t i = 0; i < sizeof(wait_test_multi); i++)
    {
        if (wait_test_multi[i] != wait_test_single[i])
        {
            panic("Multi-sector read returned different data");
        }
    }

    uart_puts("[INFO] Wait queues verified: ");
    uart_put_uint(WAIT_TEST_READS);
    uart_puts(" disk reads, the reader slept ");
    uart_put_uint(blocks);
    uart_puts(" times and another process ran meanwhile\n");

    if (blocks == 0)
    {
        log_info("(the disk answered every read instantly, so there was nothing to wait for)");
    }
    log_info("Multi-sector disk read verified: 8 sectors in one request");
}

static const char *user_test_programs[] = {"hello", "badcall", "noperm", "hog", "bigmem", "files", "modelcheck"};

#define USER_TEST_COUNT (sizeof(user_test_programs) / sizeof(user_test_programs[0]))

static void run_user_program(const char *name)
{
    int pid = process_spawn(program_find(name));
    int exit_code = -1;

    if (pid < 0)
    {
        panic("Could not start a user program");
    }

    if (process_wait(pid, &exit_code) != 0 || exit_code != 0)
    {
        uart_puts("[WARN] User program failed: ");
        uart_puts(name);
        uart_puts(", exit code ");
        uart_put_uint((uint64_t)exit_code);
        uart_putc('\n');
        panic("User mode test failed");
    }
}

static void user_mode_test(void)
{
    /* The first run may grow the kernel heap (process stacks), so count
       free pages only around the second run */
    run_user_program("hello");

    unsigned long free_before = page_free_count();

    for (uint32_t i = 0; i < USER_TEST_COUNT; i++)
    {
        run_user_program(user_test_programs[i]);
    }

    if (page_free_count() != free_before)
    {
        log_info_uint("Pages lost: ", free_before - page_free_count());
        panic("User programs leaked memory");
    }

    log_info("User memory verified: every page and page table was returned when the programs exited");

    if (knocfs_mounted())
    {
        if (process_disk_loads() == 0)
        {
            panic("Programs were not loaded from the disk");
        }

        log_info_uint("Programs loaded from /bin on disk: ", process_disk_loads());
    }

    log_info("User mode verified: 7 programs ran in U-mode with system calls, bad pointers refused, capabilities and quotas enforced");
}

static void start_program(const char *name)
{
    if (process_spawn(program_find(name)) < 0)
    {
        log_warn("Could not start the program");
    }
}

static void console_process(void *arg)
{
    (void)arg;

    log_info("Console ready (Ctrl-D power off, Ctrl-C stop the running program)");
    log_info("Test keys: Ctrl-F process fault, Ctrl-X driver fault, Ctrl-K kernel fault, Ctrl-O overwrite kernel code, Ctrl-P panic, Ctrl-W freeze");
    log_info("Program keys: Ctrl-U run the crash program, Ctrl-E run the spy program");

    /* After a crash the AI restarts the console: the shell may still be running */
    if (!health_started)
    {
        health_started = 1;
        process_spawn(program_find("healthd"));
    }

    if (!tty_has_owner() && process_spawn(program_find("knocsh")) < 0)
    {
        log_warn("No shell: the console only echoes keys");
    }

    while (1)
    {
        char c;

        if (device_read(console, &c, 1) <= 0)
        {
            uart_wait_input();
            continue;
        }

        if (c == KEY_CTRL_P)
        {
            device_write(console, "\n", 1);
            panic("Test panic (Ctrl-P)");
        }
        else if (c == KEY_CTRL_O)
        {
            device_write(console, "\n", 1);
            log_info("Test code corruption (Ctrl-O): overwriting log_info() with zeros, then calling it");
            asm volatile("csrc sstatus, %0" :: "r"((uint64_t)SSTATUS_SIE));

            volatile uint32_t *code = (volatile uint32_t *)(uintptr_t)log_info;

            for (int i = 0; i < 4; i++)
            {
                code[i] = 0;
            }

            asm volatile("fence.i");
            log_info("unreachable");
        }
        else if (c == KEY_CTRL_U)
        {
            device_write(console, "\n", 1);
            log_info("Test user crash (Ctrl-U): starting the crash program");
            start_program("crash");
        }
        else if (c == KEY_CTRL_E)
        {
            device_write(console, "\n", 1);
            log_info("Test user security (Ctrl-E): starting the spy program");
            start_program("spy");
        }
        else if (c == KEY_CTRL_F)
        {
            device_write(console, "\n", 1);
            log_info("Test process fault (Ctrl-F): the console writes to an unmapped address");
            *(volatile uint64_t *)TEST_FAULT_ADDRESS = 1;
        }
        else if (c == KEY_CTRL_X)
        {
            device_write(console, "\n", 1);
            log_info("Test driver fault (Ctrl-X): the faulty0 driver writes to an unmapped address");

            if (device_write(faulty, "x", 1) < 0)
            {
                log_info("faulty0 is disabled, nothing happened");
            }
        }
        else if (c == KEY_CTRL_K)
        {
            device_write(console, "\n", 1);
            log_info("Test kernel fault (Ctrl-K): bad pointer with interrupts off");
            asm volatile("csrc sstatus, %0" :: "r"((uint64_t)SSTATUS_SIE));
            *(volatile uint64_t *)TEST_FAULT_ADDRESS = 1;
        }
        else if (c == KEY_CTRL_W)
        {
            device_write(console, "\n", 1);
            log_info("Test freeze (Ctrl-W): interrupts off, infinite loop");
            asm volatile("csrc sstatus, %0" :: "r"((uint64_t)SSTATUS_SIE));

            while (1)
            {
            }
        }
        else if (c == KEY_CTRL_C)
        {
            int foreground = tty_foreground();

            if (foreground != 0)
            {
                device_write(console, "^C\n", 3);
                process_kill(foreground);
            }
        }
        else if (c == KEY_CTRL_D)
        {
            uint8_t command = POWER_COMMAND_OFF;

            device_write(console, "\n", 1);
            log_info("Powering off");
            device_write(power, &command, 1);
        }
        else if (tty_has_owner())
        {
            /* The shell gets every other key and echoes it itself */
            tty_input(c);
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

    wait_queue_test();
    user_mode_test();

    log_info("All self-tests passed");

    if (process_create("console", PROCESS_CLASS_INTERACTIVE, console_process, 0) < 0)
    {
        panic("Could not create console process");
    }
}

void kernel_main(uintptr_t dtb)
{
    log_info("KnocOS " KNOCOS_VERSION " starting");

    trap_enable_interrupts();
    log_info("Supervisor interrupts enabled");

    fdt_info_t fdt;

    if (dtb >= AISPACE_BASE && dtb < AISPACE_BASE + AISPACE_SIZE)
    {
        panic("Device tree is inside the AI space: KnocOS needs at least 1 GiB of RAM");
    }

    if (fdt_parse(dtb, &fdt) != 0)
    {
        panic("No valid device tree from the firmware");
    }

    uart_puts("[INFO] Device tree at ");
    uart_put_hex(fdt.dtb_start);
    print_mib(": RAM ", fdt.ram_size);
    uart_puts(" at ");
    uart_put_hex(fdt.ram_start);
    uart_puts(", ");
    uart_put_uint(fdt.cpu_count);
    uart_puts(" CPUs\n");

    if (fdt.ram_start + fdt.ram_size < AISPACE_BASE + AISPACE_SIZE)
    {
        panic("Not enough RAM: KnocOS needs at least 1 GiB (QEMU -m 1G)");
    }

    if (page_init(fdt.ram_start, fdt.ram_size, fdt.dtb_start, fdt.dtb_size) != 0)
    {
        panic("Page allocator setup failed");
    }

    print_mib("[INFO] Page memory initialized: ", page_free_count() * PAGE_SIZE);
    print_mib(" free, largest block ", page_largest_free() * PAGE_SIZE);
    uart_puts(" (buddy allocator)\n");

    vm_init(fdt.ram_start, fdt.ram_start + fdt.ram_size);
    log_info("Virtual memory initialized");

    log_info("Kernel page tables ready");

    heap_init();
    log_info("Kernel heap mapping prepared");

    unsigned long pages_before =
        page_used();

    log_info("Enabling Sv39");

    vm_enable();

    log_info("Sv39 enabled");
    log_info_uint("RAM mapped with 2 MiB megapages: ", vm_megapage_count());

    heap_activate();

    log_info("Kernel heap activated");

    uint64_t probe_value;

    if (trap_probe_read((uintptr_t)&probe_value, &probe_value) != 0)
    {
        panic("Probe of kernel memory failed");
    }

    if (trap_probe_read(AISPACE_BASE, &probe_value) == 0)
    {
        panic("AI space memory is not protected");
    }

    log_info("PMP verified: the kernel cannot read the AI space");

    guardian_init();
    guardian_set_ram_end(fdt.ram_start + fdt.ram_size);
    spinlock_self_test();

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

    memory_self_test();

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
    faulty_register();

    device_init_all();
    device_list();

    console = device_find("uart0");
    power = device_find("power0");
    faulty = device_find("faulty0");

    if (console == 0 || power == 0)
    {
        panic("Required device missing");
    }

    if (device_count() != 4 || device_find("missing0") != 0)
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

    if (disk != 0 && disk->ready && knocfs_mount(disk) == 0)
    {
        uint64_t total_bytes;
        uint64_t free_bytes;
        uint32_t files;

        knocfs_usage(&total_bytes, &free_bytes, &files);

        print_mib("[INFO] KnocFS mounted on disk0: ", total_bytes);
        uart_puts(", ");
        uart_put_uint(files);
        print_mib(" files, ", free_bytes);
        uart_puts(" free\n");

        graph_stats_t graph;

        if (memgraph_init(boot_number) == 0 && memgraph_stats(&graph) == 0)
        {
            uart_puts("[INFO] Memory graph ready: ");
            uart_put_uint(graph.nodes);
            uart_puts(" nodes, ");
            uart_put_uint(graph.edges);
            uart_puts(" links (boot ");
            uart_put_uint(boot_number);
            uart_puts(")\n");
        }
        else
        {
            log_warn("Memory graph unavailable");
        }
    }
    else
    {
        log_warn("No KnocFS on disk0: programs run from the kernel's built-in copies (make reset-disk)");
    }

    uint64_t consecutive_crashes = guardian_boot_report(disk);
    int safe_mode = consecutive_crashes >= BLACKBOX_SAFE_MODE_THRESHOLD ||
                    guardian_restart_safe_mode();

    guardian_set_safe_mode(safe_mode);

    process_init();

    if (safe_mode)
    {
        log_warn("SAFE MODE: several crashes in a row, starting minimal services only");
        log_info("All self-tests skipped in safe mode");

        if (process_create("console", PROCESS_CLASS_INTERACTIVE, console_process, 0) < 0)
        {
            panic("Could not create console process");
        }
    }
    else if (process_create("sched-test", PROCESS_CLASS_INTERACTIVE, scheduler_test, 0) < 0)
    {
        panic("Could not create scheduler test process");
    }

    if (process_create("telemetry", PROCESS_CLASS_BACKGROUND, telemetry_process, 0) < 0)
    {
        panic("Could not create telemetry process");
    }

    if (process_create("guardian", PROCESS_CLASS_BACKGROUND, guardian_process, 0) < 0)
    {
        panic("Could not create guardian process");
    }

    scheduler_start();
    guardian_start_watch();

    while (1)
    {
        asm volatile("wfi");
    }
}

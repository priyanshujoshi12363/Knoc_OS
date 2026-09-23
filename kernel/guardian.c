#include "guardian.h"
#include "mailbox.h"
#include "blackbox.h"
#include "aispace.h"
#include "timer.h"
#include "uart.h"
#include "logging.h"
#include "process.h"

#define GUARDIAN_ONLINE_TIMEOUT TIMER_FREQ_HZ

guardian_mailbox_t guardian_mailbox __attribute__((section(".mailbox")));

static uint8_t guardian_sector[BLACKBOX_SECTOR_SIZE];
static device_t *guardian_disk;

static void copy_text(volatile char *destination, const char *source, int max)
{
    int i = 0;

    while (i < max - 1 && source[i])
    {
        destination[i] = source[i];
        i++;
    }

    destination[i] = 0;
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

static void zero_bytes(void *destination, uint64_t length)
{
    uint8_t *to = (uint8_t *)destination;

    for (uint64_t i = 0; i < length; i++)
    {
        to[i] = 0;
    }
}

int guardian_init(void)
{
    uint64_t start = timer_read();

    guardian_set_current(0, "kernel-boot");

    while (guardian_mailbox.aispace_state != AISPACE_STATE_ONLINE ||
           guardian_mailbox.aispace_magic != MAILBOX_MAGIC)
    {
        if (timer_read() - start > GUARDIAN_ONLINE_TIMEOUT)
        {
            log_warn("AI space offline: crashes will not be recorded (run with -smp 2)");
            return -1;
        }
    }

    uart_puts("[INFO] AI space online (core ");
    uart_put_uint(AISPACE_HART);
    uart_puts(", ");
    uart_put_uint(AISPACE_SIZE / (1024 * 1024));
    uart_puts(" MiB protected at ");
    uart_put_hex(AISPACE_BASE);
    uart_puts(")\n");

    return 0;
}

void guardian_heartbeat(void)
{
    guardian_mailbox.uptime_ticks = timer_ticks();
    guardian_mailbox.heartbeat = guardian_mailbox.heartbeat + 1;
}

void guardian_set_current(int pid, const char *name)
{
    guardian_mailbox.current_pid = pid;
    copy_text(guardian_mailbox.current_name, name, MAILBOX_NAME_MAX);
}

void guardian_start_watch(void)
{
    guardian_mailbox.kernel_state = KERNEL_STATE_RUNNING;
    guardian_mailbox.watch_enabled = 1;
}

void guardian_set_safe_mode(int enabled)
{
    guardian_mailbox.safe_mode = (uint32_t)enabled;
}

void guardian_record_trap(uint64_t scause,
                          uint64_t sepc,
                          uint64_t stval,
                          uint64_t ra,
                          uint64_t sp)
{
    guardian_mailbox.scause = scause;
    guardian_mailbox.sepc = sepc;
    guardian_mailbox.stval = stval;
    guardian_mailbox.ra = ra;
    guardian_mailbox.sp = sp;
    guardian_mailbox.crash_type = CRASH_TYPE_TRAP;
}

void guardian_report_panic(const char *message)
{
    if (guardian_mailbox.crash_type != CRASH_TYPE_TRAP)
    {
        guardian_mailbox.crash_type = CRASH_TYPE_PANIC;
    }

    copy_text(guardian_mailbox.message, message, MAILBOX_MESSAGE_MAX);

    __sync_synchronize();

    guardian_mailbox.kernel_state = KERNEL_STATE_PANICKED;
}

static const char *crash_type_name(uint32_t type)
{
    if (type == CRASH_TYPE_PANIC)
    {
        return "PANIC";
    }

    if (type == CRASH_TYPE_TRAP)
    {
        return "TRAP";
    }

    if (type == CRASH_TYPE_FREEZE)
    {
        return "FREEZE";
    }

    return "UNKNOWN";
}

static const char *action_name(uint32_t action)
{
    if (action == BLACKBOX_ACTION_REBOOT)
    {
        return "reboot";
    }

    if (action == BLACKBOX_ACTION_SAFE_MODE)
    {
        return "reboot into safe mode";
    }

    return "halt";
}

static void print_record(blackbox_record_t *record)
{
    uart_puts("[WARN] Previous crash detected: #");
    uart_put_uint(record->sequence);
    uart_puts(" ");
    uart_puts(crash_type_name(record->crash_type));
    uart_puts(" - ");
    uart_puts(record->message);
    uart_putc('\n');

    uart_puts("[WARN]   process: ");
    uart_puts(record->process_name);
    uart_puts(" (pid ");
    uart_put_uint((uint64_t)record->pid);
    uart_puts("), uptime ticks: ");
    uart_put_uint(record->uptime_ticks);
    uart_putc('\n');

    if (record->crash_type == CRASH_TYPE_TRAP)
    {
        uart_puts("[WARN]   scause = ");
        uart_put_hex(record->scause);
        uart_puts("  sepc = ");
        uart_put_hex(record->sepc);
        uart_puts("  stval = ");
        uart_put_hex(record->stval);
        uart_putc('\n');
    }
    else if (record->crash_type == CRASH_TYPE_FREEZE)
    {
        uart_puts("[WARN]   kernel_pc = ");
        uart_put_hex(record->kernel_pc);
        uart_putc('\n');
    }

    uart_puts("[WARN]   AI diagnosis: ");
    uart_puts(record->diagnosis);
    uart_putc('\n');

    uart_puts("[WARN]   AI action: ");
    uart_puts(action_name(record->action));
    uart_putc('\n');
}

static int read_header(blackbox_header_t *header)
{
    if (device_read_block(guardian_disk,
                          blackbox_header_sector(guardian_disk->block_count),
                          guardian_sector) != 0)
    {
        return -1;
    }

    copy_bytes(header, guardian_sector, sizeof(*header));

    if (header->magic != BLACKBOX_MAGIC)
    {
        return -1;
    }

    return 0;
}

static int write_header(blackbox_header_t *header)
{
    zero_bytes(guardian_sector, BLACKBOX_SECTOR_SIZE);
    copy_bytes(guardian_sector, header, sizeof(*header));

    return device_write_block(guardian_disk,
                              blackbox_header_sector(guardian_disk->block_count),
                              guardian_sector);
}

uint64_t guardian_boot_report(device_t *disk)
{
    blackbox_header_t header;
    blackbox_record_t record;

    guardian_disk = disk;

    if (disk == 0 || !disk->ready || disk->block_count <= BLACKBOX_SECTORS)
    {
        guardian_disk = 0;
        return 0;
    }

    if (read_header(&header) != 0)
    {
        log_info("Black box: no previous crashes");
        return 0;
    }

    uint64_t first = header.reported_crashes + 1;

    if (header.total_crashes > BLACKBOX_RECORDS &&
        first < header.total_crashes - BLACKBOX_RECORDS + 1)
    {
        first = header.total_crashes - BLACKBOX_RECORDS + 1;
    }

    for (uint64_t sequence = first; sequence <= header.total_crashes; sequence++)
    {
        if (device_read_block(disk,
                              blackbox_record_sector(disk->block_count, sequence),
                              guardian_sector) != 0)
        {
            continue;
        }

        copy_bytes(&record, guardian_sector, sizeof(record));

        if (record.magic == BLACKBOX_MAGIC && record.sequence == sequence)
        {
            print_record(&record);
        }
    }

    if (header.total_crashes == header.reported_crashes)
    {
        log_info_uint("Black box: no new crashes, total recorded: ", header.total_crashes);
    }

    header.reported_crashes = header.total_crashes;
    write_header(&header);

    return header.consecutive_crashes;
}

void guardian_healthy_process(void *arg)
{
    (void)arg;

    process_sleep(GUARDIAN_HEALTHY_TICKS);

    blackbox_header_t header;

    if (guardian_disk == 0 || read_header(&header) != 0)
    {
        return;
    }

    if (header.consecutive_crashes != 0)
    {
        header.consecutive_crashes = 0;
        write_header(&header);
        log_info("Guardian: 60 s without a crash, crash streak reset");
    }
}

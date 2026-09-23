#include "guardian.h"
#include "mailbox.h"
#include "blackbox.h"
#include "aispace.h"
#include "timer.h"
#include "uart.h"
#include "logging.h"
#include "process.h"
#include "syscall_abi.h"

#define GUARDIAN_ONLINE_TIMEOUT TIMER_FREQ_HZ

guardian_mailbox_t guardian_mailbox __attribute__((section(".mailbox")));

static uint8_t guardian_sector[BLACKBOX_SECTOR_SIZE];
static device_t *guardian_disk;
static int guardian_online;

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

static int names_equal(const volatile char *a, const char *b)
{
    int i = 0;

    while (a[i] && a[i] == b[i])
    {
        i++;
    }

    return a[i] == b[i];
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

    guardian_online = 1;

    if (guardian_mailbox.kernel_restarts > 0)
    {
        uart_puts("[INFO] Warm restart #");
        uart_put_uint(guardian_mailbox.kernel_restarts);
        uart_puts(" by the AI space: kernel restored from a clean copy, the AI kept running (AI uptime ");
        uart_put_uint((timer_read() - guardian_mailbox.ai_start_time) / (TIMER_FREQ_HZ / 1000));
        uart_puts(" ms)\n");
    }

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

void guardian_set_ram_end(uint64_t ram_end)
{
    guardian_mailbox.ram_end = ram_end;
}

int guardian_restart_safe_mode(void)
{
    return guardian_online && guardian_mailbox.restart_safe_mode;
}

int guardian_driver_disabled(const char *name)
{
    if (!guardian_online)
    {
        return 0;
    }

    for (int i = 0; i < MAILBOX_DISABLED_MAX; i++)
    {
        if (guardian_mailbox.disabled_drivers[i][0] &&
            names_equal(guardian_mailbox.disabled_drivers[i], name))
        {
            return 1;
        }
    }

    return 0;
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
    copy_text(guardian_mailbox.driver, process_current_driver() ? process_current_driver() : "", MAILBOX_NAME_MAX);
    guardian_mailbox.crash_type = CRASH_TYPE_TRAP;
}

void guardian_report_panic(const char *message)
{
    if (guardian_mailbox.crash_type != CRASH_TYPE_TRAP)
    {
        guardian_mailbox.crash_type = CRASH_TYPE_PANIC;
        copy_text(guardian_mailbox.driver, process_current_driver() ? process_current_driver() : "", MAILBOX_NAME_MAX);
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

    if (action == BLACKBOX_ACTION_RESTART)
    {
        return "warm kernel restart";
    }

    if (action == BLACKBOX_ACTION_RESTART_SAFE)
    {
        return "warm kernel restart into safe mode";
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

    if (record->driver[0])
    {
        uart_puts("[WARN]   inside driver: ");
        uart_puts(record->driver);
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

static void reset_streak(void)
{
    blackbox_header_t header;

    guardian_mailbox.streak_reset = 1;

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

static void post_fault(process_fault_t *fault)
{
    guardian_mailbox.fault_pid = fault->pid;
    guardian_mailbox.fault_restarts = fault->restarts;
    copy_text(guardian_mailbox.fault_name, fault->name, MAILBOX_NAME_MAX);
    copy_text(guardian_mailbox.fault_driver, fault->driver, MAILBOX_NAME_MAX);
    guardian_mailbox.fault_scause = fault->scause;
    guardian_mailbox.fault_sepc = fault->sepc;
    guardian_mailbox.fault_stval = fault->stval;
    guardian_mailbox.fault_user = (uint32_t)fault->user;
    guardian_mailbox.fault_denied = fault->denied;
    guardian_mailbox.fault_trace_count = fault->trace_count;

    for (uint32_t i = 0; i < fault->trace_count && i < MAILBOX_TRACE_MAX; i++)
    {
        guardian_mailbox.fault_trace[i] = fault->trace[i];
    }

    __sync_synchronize();

    guardian_mailbox.fault_seq = guardian_mailbox.fault_seq + 1;
}

static uint32_t default_verdict(process_fault_t *fault)
{
    uint32_t action = 0;

    if (fault->driver[0])
    {
        action |= VERDICT_DISABLE_DRIVER;
    }

    if (fault->restarts < GUARDIAN_RESTART_LIMIT && fault->denied == 0)
    {
        action |= VERDICT_RESTART_PROCESS;
    }

    return action;
}

static void apply_verdict(process_fault_t *fault, uint32_t action, const char *source)
{
    if ((action & VERDICT_DISABLE_DRIVER) && fault->driver[0])
    {
        device_disable(device_find(fault->driver));

        uart_puts("[INFO] Driver ");
        uart_puts(fault->driver);
        uart_puts(" disabled (");
        uart_puts(source);
        uart_puts(")\n");
    }

    if (action & VERDICT_RESTART_PROCESS)
    {
        int pid = process_restart(fault->pid);

        uart_puts("[INFO] Process ");
        uart_puts(fault->name);
        uart_puts(" restarted as pid ");
        uart_put_uint((uint64_t)pid);
        uart_puts(", restart #");
        uart_put_uint(fault->restarts + 1);
        uart_puts(" (");
        uart_puts(source);
        uart_puts(")\n");
    }
    else
    {
        process_discard(fault->pid);

        uart_puts("[WARN] Process ");
        uart_puts(fault->name);
        uart_puts(fault->denied > 0 ? " left stopped: suspicious, it made forbidden system calls ("
                                    : " left stopped: it keeps crashing (");
        uart_puts(source);
        uart_puts(")\n");
    }
}

void guardian_process(void *arg)
{
    (void)arg;

    uint64_t start = timer_ticks();
    uint64_t posted_at = 0;
    int streak_checked = 0;
    int pending = 0;
    process_fault_t fault;

    while (1)
    {
        /* Woken at once by a crash; while waiting for the AI's verdict,
           check the mailbox every tick */
        process_wait_crash(pending ? 1 : GUARDIAN_POLL_TICKS);

        if (!pending && process_next_crash(&fault) == 0)
        {
            pending = 1;
            posted_at = timer_ticks();

            if (guardian_online)
            {
                post_fault(&fault);
            }
        }

        if (pending)
        {
            if (guardian_online &&
                guardian_mailbox.verdict_seq == guardian_mailbox.fault_seq)
            {
                __sync_synchronize();
                apply_verdict(&fault, guardian_mailbox.verdict_action, "AI verdict");
                pending = 0;
            }
            else if (!guardian_online ||
                     timer_ticks() - posted_at > GUARDIAN_VERDICT_TIMEOUT)
            {
                apply_verdict(&fault, default_verdict(&fault), "kernel default, no AI verdict");
                pending = 0;
            }
        }

        if (!streak_checked && timer_ticks() - start >= GUARDIAN_HEALTHY_TICKS)
        {
            streak_checked = 1;
            reset_streak();
        }
    }
}

/* ---- For the shell: AI space status and crash reports ---- */

void guardian_system_info(system_info_t *info)
{
    blackbox_header_t header;

    info->ai_online = guardian_online && guardian_mailbox.aispace_magic == MAILBOX_MAGIC;
    info->kernel_restarts = info->ai_online ? guardian_mailbox.kernel_restarts : 0;
    info->ai_uptime_ms = info->ai_online
                             ? (timer_read() - guardian_mailbox.ai_start_time) / (TIMER_FREQ_HZ / 1000)
                             : 0;
    info->safe_mode = guardian_mailbox.safe_mode;

    for (int i = 0; i < INFO_DISABLED_MAX && i < MAILBOX_DISABLED_MAX; i++)
    {
        copy_bytes(info->disabled_drivers[i], (const void *)guardian_mailbox.disabled_drivers[i], INFO_NAME_MAX);
        info->disabled_drivers[i][INFO_NAME_MAX - 1] = 0;

        if (!info->ai_online)
        {
            info->disabled_drivers[i][0] = 0;
        }
    }

    info->crashes_total = 0;
    info->crashes_in_a_row = 0;

    if (guardian_disk != 0 && read_header(&header) == 0)
    {
        info->crashes_total = (uint32_t)header.total_crashes;
        info->crashes_in_a_row = (uint32_t)header.consecutive_crashes;
    }
}

/* index 0 = the newest crash; the black box keeps the last 4 */
int guardian_crash_info(uint32_t index, crash_info_t *info)
{
    blackbox_header_t header;
    blackbox_record_t record;

    if (guardian_disk == 0 || read_header(&header) != 0 ||
        index >= BLACKBOX_RECORDS || index >= header.total_crashes)
    {
        return -1;
    }

    uint64_t sequence = header.total_crashes - index;

    if (device_read_block(guardian_disk,
                          blackbox_record_sector(guardian_disk->block_count, sequence),
                          guardian_sector) != 0)
    {
        return -1;
    }

    copy_bytes(&record, guardian_sector, sizeof(record));

    if (record.magic != BLACKBOX_MAGIC || record.sequence != sequence)
    {
        return -1;
    }

    zero_bytes(info, sizeof(*info));
    info->sequence = record.sequence;
    info->uptime_ticks = record.uptime_ticks;
    info->crash_type = record.crash_type;
    info->action = record.action;
    info->pid = record.pid;
    copy_bytes(info->process, record.process_name, INFO_NAME_MAX);
    copy_bytes(info->driver, record.driver, INFO_NAME_MAX);
    copy_bytes(info->message, record.message, sizeof(info->message));
    copy_bytes(info->diagnosis, record.diagnosis, sizeof(info->diagnosis));

    return 0;
}

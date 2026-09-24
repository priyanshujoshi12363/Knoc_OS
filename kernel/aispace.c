#include "aispace.h"
#include "mailbox.h"
#include "blackbox.h"
#include "virtio.h"
#include "syscall_abi.h"

#define AI_UART 0x10000000UL
#define AI_UART_LSR 5
#define AI_UART_THR_EMPTY 0x20

#define AI_MTIME 0x0200BFF8UL
#define AI_TIMER_HZ 10000000ULL
#define AI_POLL_INTERVAL (AI_TIMER_HZ / 100)
#define AI_FREEZE_TIMEOUT (AI_TIMER_HZ * 2)
#define AI_DISK_TIMEOUT AI_TIMER_HZ
#define AI_REBOOT_DELAY (AI_TIMER_HZ / 2)
#define AI_PARK_TIMEOUT (AI_TIMER_HZ / 2)
#define AI_PROCESS_RESTART_LIMIT 3
#define AI_CONSOLE_TIMEOUT (AI_TIMER_HZ / 100)

#define AI_POWER 0x00100000UL
#define AI_POWER_REBOOT 0x7777

#define AI_RAM_START 0x80000000UL
#define AI_RAM_DEFAULT_END 0xC0000000UL
#define AI_NULL_LIMIT 0x1000UL

#define AI_QUEUE_SIZE 8
#define AI_BLK_T_IN 0
#define AI_BLK_T_OUT 1
#define AI_BLK_STATUS_PENDING 0xFF

#define AI_SCAUSE_CODE_MASK 0x7FFFFFFFFFFFFFFFULL

typedef struct ai_blk_request
{
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} ai_blk_request_t;

typedef struct ai_avail
{
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[AI_QUEUE_SIZE];
    uint16_t used_event;
} ai_avail_t;

typedef struct ai_used
{
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[AI_QUEUE_SIZE];
    uint16_t avail_event;
} ai_used_t;

static virtq_desc_t ai_desc[AI_QUEUE_SIZE] __attribute__((aligned(4096)));
static ai_avail_t ai_avail __attribute__((aligned(4096)));
static ai_used_t ai_used __attribute__((aligned(4096)));

static ai_blk_request_t ai_request;
static uint8_t ai_sector[BLACKBOX_SECTOR_SIZE] __attribute__((aligned(16)));
static volatile uint8_t ai_status;
static uint16_t ai_used_seen;
static uint64_t ai_disk_capacity;

static blackbox_record_t ai_record;
static blackbox_record_t ai_fault_record;

extern char _start[];
extern char kernel_start[];
extern char kernel_code_end[];
extern char kernel_image_end[];
extern char aispace_snapshot[];
extern char aispace_snapshot_end[];

static uintptr_t ai_dtb;
static uint64_t ai_image_size;
static int ai_have_snapshot;
static uint64_t ai_start_time;
static uint32_t ai_restarts;
static uint64_t ai_consecutive;
static int ai_kernel_halted;
static char ai_disabled[MAILBOX_DISABLED_MAX][MAILBOX_NAME_MAX];

_Static_assert(__builtin_offsetof(guardian_mailbox_t, boot_request) == MAILBOX_BOOT_REQUEST,
               "boot.S expects boot_request at offset 0");
_Static_assert(__builtin_offsetof(guardian_mailbox_t, boot_ack) == MAILBOX_BOOT_ACK,
               "boot.S expects boot_ack at offset 8");
_Static_assert(sizeof(blackbox_record_t) <= BLACKBOX_SECTOR_SIZE,
               "a black box record must fit in one sector");

static uint64_t ai_time(void);

static int ai_line_open;

static void ai_line_begin(void)
{
    uint64_t start = ai_time();

    while (1)
    {
        uint32_t expected = CONSOLE_FREE;

        if (__atomic_compare_exchange_n(&guardian_mailbox.console_owner,
                                        &expected,
                                        CONSOLE_AISPACE,
                                        0,
                                        __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
        {
            break;
        }

        /* The kernel may have crashed in the middle of a line */
        if (ai_time() - start > AI_CONSOLE_TIMEOUT)
        {
            break;
        }
    }

    ai_line_open = 1;
}

static void ai_line_end(void)
{
    uint32_t expected = CONSOLE_AISPACE;

    __atomic_compare_exchange_n(&guardian_mailbox.console_owner,
                                &expected,
                                CONSOLE_FREE,
                                0,
                                __ATOMIC_RELEASE,
                                __ATOMIC_RELAXED);

    ai_line_open = 0;
}

static void ai_putc(char c)
{
    volatile uint8_t *uart = (volatile uint8_t *)AI_UART;

    if (!ai_line_open)
    {
        ai_line_begin();
    }

    while (!(uart[AI_UART_LSR] & AI_UART_THR_EMPTY))
    {
    }

    uart[0] = (uint8_t)c;

    if (c == '\n')
    {
        ai_line_end();
    }
}

static void ai_puts(const char *text)
{
    while (*text)
    {
        ai_putc(*text);
        text++;
    }
}

static void ai_put_hex(uint64_t value)
{
    const char *digits = "0123456789ABCDEF";

    ai_puts("0x");

    for (int i = 15; i >= 0; i--)
    {
        ai_putc(digits[(value >> (i * 4)) & 0xF]);
    }
}

static void ai_put_uint(uint64_t value)
{
    char buffer[20];
    int i = 0;

    if (value == 0)
    {
        ai_putc('0');
        return;
    }

    while (value > 0)
    {
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    }

    while (i > 0)
    {
        ai_putc(buffer[--i]);
    }
}

static void ai_log(const char *text)
{
    ai_puts("[AI] ");
    ai_puts(text);
    ai_putc('\n');
}

static uint64_t ai_time(void)
{
    return *(volatile uint64_t *)AI_MTIME;
}

static void ai_wait(uint64_t counts)
{
    uint64_t start = ai_time();

    while (ai_time() - start < counts)
    {
    }
}

static void ai_zero(void *destination, uint64_t length)
{
    volatile uint8_t *to = (volatile uint8_t *)destination;

    for (uint64_t i = 0; i < length; i++)
    {
        to[i] = 0;
    }
}

static void ai_copy(void *destination, const void *source, uint64_t length)
{
    volatile uint8_t *to = (volatile uint8_t *)destination;
    const volatile uint8_t *from = (const volatile uint8_t *)source;

    for (uint64_t i = 0; i < length; i++)
    {
        to[i] = from[i];
    }
}

static void ai_copy_text(char *destination, const volatile char *source, int max)
{
    int i = 0;

    while (i < max - 1 && source[i])
    {
        destination[i] = source[i];
        i++;
    }

    destination[i] = 0;
}

static int ai_text_equal(const volatile char *a, const volatile char *b)
{
    int i = 0;

    while (a[i] && a[i] == b[i])
    {
        i++;
    }

    return a[i] == b[i];
}

static void ai_append(char *destination, const char *text, int max)
{
    int length = 0;

    while (destination[length])
    {
        length++;
    }

    while (*text && length < max - 1)
    {
        destination[length++] = *text++;
    }

    destination[length] = 0;
}

static uint32_t ai_reg_read(uint32_t reg)
{
    return *(volatile uint32_t *)(VIRTIO0_BASE + reg);
}

static void ai_reg_write(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(VIRTIO0_BASE + reg) = value;
}

static int ai_disk_init(void)
{
    if (ai_reg_read(VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MAGIC ||
        ai_reg_read(VIRTIO_MMIO_VERSION) != VIRTIO_VERSION_MODERN ||
        ai_reg_read(VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_BLOCK)
    {
        return -1;
    }

    ai_reg_write(VIRTIO_MMIO_STATUS, 0);

    uint64_t start = ai_time();

    while (ai_reg_read(VIRTIO_MMIO_STATUS) != 0)
    {
        if (ai_time() - start > AI_DISK_TIMEOUT)
        {
            return -1;
        }
    }

    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE;
    ai_reg_write(VIRTIO_MMIO_STATUS, status);

    status |= VIRTIO_STATUS_DRIVER;
    ai_reg_write(VIRTIO_MMIO_STATUS, status);

    ai_reg_write(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);

    if (!(ai_reg_read(VIRTIO_MMIO_DEVICE_FEATURES) & VIRTIO_F_VERSION_1_HIGH_BIT))
    {
        return -1;
    }

    ai_reg_write(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    ai_reg_write(VIRTIO_MMIO_DRIVER_FEATURES, 0);
    ai_reg_write(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    ai_reg_write(VIRTIO_MMIO_DRIVER_FEATURES, VIRTIO_F_VERSION_1_HIGH_BIT);

    status |= VIRTIO_STATUS_FEATURES_OK;
    ai_reg_write(VIRTIO_MMIO_STATUS, status);

    if (!(ai_reg_read(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK))
    {
        return -1;
    }

    ai_reg_write(VIRTIO_MMIO_QUEUE_SEL, 0);

    if (ai_reg_read(VIRTIO_MMIO_QUEUE_NUM_MAX) < AI_QUEUE_SIZE)
    {
        return -1;
    }

    ai_zero(ai_desc, sizeof(ai_desc));
    ai_zero(&ai_avail, sizeof(ai_avail));
    ai_zero(&ai_used, sizeof(ai_used));
    ai_used_seen = 0;

    ai_reg_write(VIRTIO_MMIO_QUEUE_NUM, AI_QUEUE_SIZE);

    ai_reg_write(VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)(uintptr_t)ai_desc);
    ai_reg_write(VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)((uintptr_t)ai_desc >> 32));
    ai_reg_write(VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint32_t)(uintptr_t)&ai_avail);
    ai_reg_write(VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uint32_t)((uintptr_t)&ai_avail >> 32));
    ai_reg_write(VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint32_t)(uintptr_t)&ai_used);
    ai_reg_write(VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (uint32_t)((uintptr_t)&ai_used >> 32));

    ai_reg_write(VIRTIO_MMIO_QUEUE_READY, 1);

    status |= VIRTIO_STATUS_DRIVER_OK;
    ai_reg_write(VIRTIO_MMIO_STATUS, status);

    uint64_t capacity_low = ai_reg_read(VIRTIO_MMIO_CONFIG);
    uint64_t capacity_high = ai_reg_read(VIRTIO_MMIO_CONFIG + 4);

    ai_disk_capacity = (capacity_high << 32) | capacity_low;

    if (ai_disk_capacity <= BLACKBOX_SECTORS)
    {
        return -1;
    }

    return 0;
}

static int ai_disk_request(uint32_t type, uint64_t sector)
{
    volatile virtq_desc_t *desc = ai_desc;
    volatile ai_avail_t *avail = &ai_avail;
    volatile ai_used_t *used = &ai_used;

    ai_request.type = type;
    ai_request.reserved = 0;
    ai_request.sector = sector;
    ai_status = AI_BLK_STATUS_PENDING;

    desc[0].addr = (uintptr_t)&ai_request;
    desc[0].len = sizeof(ai_request);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next = 1;

    desc[1].addr = (uintptr_t)ai_sector;
    desc[1].len = BLACKBOX_SECTOR_SIZE;
    desc[1].flags = VIRTQ_DESC_F_NEXT;

    if (type == AI_BLK_T_IN)
    {
        desc[1].flags |= VIRTQ_DESC_F_WRITE;
    }

    desc[1].next = 2;

    desc[2].addr = (uintptr_t)&ai_status;
    desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next = 0;

    avail->ring[avail->idx % AI_QUEUE_SIZE] = 0;

    __sync_synchronize();

    avail->idx = avail->idx + 1;

    __sync_synchronize();

    ai_reg_write(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    uint64_t start = ai_time();

    while (used->idx == ai_used_seen)
    {
        if (ai_time() - start > AI_DISK_TIMEOUT)
        {
            return -1;
        }
    }

    ai_used_seen++;

    __sync_synchronize();

    ai_reg_write(VIRTIO_MMIO_INTERRUPT_ACK,
                 ai_reg_read(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3);

    return ai_status == 0 ? 0 : -1;
}

static int ai_disk_read(uint64_t sector, void *buffer)
{
    if (ai_disk_request(AI_BLK_T_IN, sector) != 0)
    {
        return -1;
    }

    ai_copy(buffer, ai_sector, BLACKBOX_SECTOR_SIZE);
    return 0;
}

static int ai_disk_write(uint64_t sector, const void *buffer)
{
    ai_copy(ai_sector, buffer, BLACKBOX_SECTOR_SIZE);
    return ai_disk_request(AI_BLK_T_OUT, sector);
}

static const char *ai_crash_type_name(uint32_t type)
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

static const char *ai_action_name(uint32_t action)
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
        return "warm kernel restart (only core 0, the AI keeps running)";
    }

    if (action == BLACKBOX_ACTION_RESTART_SAFE)
    {
        return "warm kernel restart into safe mode";
    }

    return "halt the kernel (crash loop detected)";
}

static void ai_diagnose(blackbox_record_t *record)
{
    char *d = record->diagnosis;
    uint64_t code = record->scause & AI_SCAUSE_CODE_MASK;
    uint64_t address = record->stval;
    uint64_t ram_end = guardian_mailbox.ram_end ? guardian_mailbox.ram_end : AI_RAM_DEFAULT_END;

    d[0] = 0;

    if (record->crash_type == CRASH_TYPE_FREEZE)
    {
        ai_append(d, "Kernel stopped responding for 2 s with interrupts off: likely an infinite loop at kernel_pc.", BLACKBOX_DIAGNOSIS_MAX);
        return;
    }

    if (record->crash_type == CRASH_TYPE_PANIC)
    {
        ai_append(d, "The kernel detected an internal error and stopped itself (see message).", BLACKBOX_DIAGNOSIS_MAX);
        return;
    }

    if (code == 12 || code == 13 || code == 15)
    {
        if (address < AI_NULL_LIMIT)
        {
            ai_append(d, "Null pointer: the code used an address near 0.", BLACKBOX_DIAGNOSIS_MAX);
        }
        else if (address < AI_RAM_START || address >= ram_end)
        {
            ai_append(d, "Bad pointer: the code accessed an address that is not mapped (stval).", BLACKBOX_DIAGNOSIS_MAX);
        }
        else
        {
            ai_append(d, "Page fault inside RAM: a page that was freed or never mapped.", BLACKBOX_DIAGNOSIS_MAX);
        }
    }
    else if (code == 1 || code == 5 || code == 7)
    {
        if (address >= AISPACE_BASE && address < AISPACE_BASE + AISPACE_SIZE)
        {
            ai_append(d, "The kernel tried to touch the protected AI space; PMP blocked it.", BLACKBOX_DIAGNOSIS_MAX);
        }
        else
        {
            ai_append(d, "Access to protected or non-existent physical memory.", BLACKBOX_DIAGNOSIS_MAX);
        }
    }
    else if (code == 2)
    {
        ai_append(d, "Illegal instruction: corrupted code or a jump to a bad address.", BLACKBOX_DIAGNOSIS_MAX);
    }
    else if (code == 0 || code == 4 || code == 6)
    {
        ai_append(d, "Misaligned access: a pointer with the wrong alignment.", BLACKBOX_DIAGNOSIS_MAX);
    }
    else
    {
        ai_append(d, "Unexpected exception.", BLACKBOX_DIAGNOSIS_MAX);
    }
}

static const char *ai_syscall_name(uint8_t number)
{
    static const char *names[SYS_COUNT] = {
        [SYS_EXIT] = "exit",
        [SYS_WRITE] = "write",
        [SYS_READ] = "read",
        [SYS_GETPID] = "getpid",
        [SYS_YIELD] = "yield",
        [SYS_SLEEP] = "sleep",
        [SYS_UPTIME] = "uptime",
        [SYS_SPAWN] = "spawn",
        [SYS_MEM_ALLOC] = "mem_alloc",
        [SYS_OPEN] = "open",
        [SYS_CLOSE] = "close",
        [SYS_SEEK] = "seek",
        [SYS_STAT] = "stat",
        [SYS_READDIR] = "readdir",
        [SYS_MKDIR] = "mkdir",
        [SYS_REMOVE] = "remove",
        [SYS_WAIT] = "wait",
        [SYS_PS] = "ps",
        [SYS_KILL] = "kill",
        [SYS_SYSINFO] = "sysinfo",
        [SYS_DEVINFO] = "devinfo",
        [SYS_CRASHINFO] = "crashinfo",
        [SYS_GETARGS] = "getargs",
        [SYS_RENAME] = "rename",
        [SYS_GRAPH] = "graph",
        [SYS_TELEMETRY] = "telemetry",
    };

    return number < SYS_COUNT ? names[number] : "unknown";
}

/* A user program lives in USER_BASE..USER_END and can't touch anything
   else, so the fault address says what it tried to do */
static void ai_diagnose_user(blackbox_record_t *record)
{
    uint64_t code = record->scause & AI_SCAUSE_CODE_MASK;
    uint64_t address = record->stval;
    char *d = record->diagnosis;

    if (code != 12 && code != 13 && code != 15)
    {
        ai_diagnose(record);
        return;
    }

    d[0] = 0;

    if (address < AI_NULL_LIMIT)
    {
        ai_append(d, "Null pointer: the program used an address near 0.", BLACKBOX_DIAGNOSIS_MAX);
    }
    else if (address >= USER_BASE && address < USER_END)
    {
        ai_append(d, "Bad pointer inside the program's own space: memory it never allocated.", BLACKBOX_DIAGNOSIS_MAX);
    }
    else
    {
        ai_append(d, "The program tried to touch memory outside its own space (kernel or devices). The page table blocked it.", BLACKBOX_DIAGNOSIS_MAX);
    }
}

static void ai_collect(uint32_t crash_type)
{
    volatile guardian_mailbox_t *mailbox = &guardian_mailbox;
    blackbox_record_t *record = &ai_record;

    ai_zero(record, sizeof(*record));

    record->magic = BLACKBOX_MAGIC;
    record->crash_type = crash_type;
    record->kernel_pc = mailbox->last_kernel_pc;
    record->uptime_ticks = mailbox->uptime_ticks;
    record->pid = mailbox->current_pid;
    ai_copy_text(record->process_name, mailbox->current_name, BLACKBOX_NAME_MAX);
    ai_copy_text(record->driver, mailbox->driver, BLACKBOX_NAME_MAX);

    if (crash_type == CRASH_TYPE_FREEZE)
    {
        ai_copy_text(record->message, "Kernel heartbeat lost", BLACKBOX_MESSAGE_MAX);
        return;
    }

    record->scause = mailbox->scause;
    record->sepc = mailbox->sepc;
    record->stval = mailbox->stval;
    record->ra = mailbox->ra;
    record->sp = mailbox->sp;
    ai_copy_text(record->message, mailbox->message, BLACKBOX_MESSAGE_MAX);
}

static void ai_print_report(blackbox_record_t *record)
{
    ai_puts("[AI] Kernel ");
    ai_puts(record->crash_type == CRASH_TYPE_FREEZE ? "freeze" : "crash");
    ai_puts(" detected: ");
    ai_puts(ai_crash_type_name(record->crash_type));
    ai_puts(" - ");
    ai_puts(record->message);
    ai_putc('\n');

    ai_puts("[AI]   process: ");
    ai_puts(record->process_name);
    ai_puts(" (pid ");
    ai_put_uint((uint64_t)record->pid);
    ai_puts("), uptime ticks: ");
    ai_put_uint(record->uptime_ticks);
    ai_putc('\n');

    if (record->crash_type == CRASH_TYPE_FREEZE)
    {
        ai_puts("[AI]   kernel_pc = ");
        ai_put_hex(record->kernel_pc);
        ai_putc('\n');
    }
    else if (record->crash_type == CRASH_TYPE_TRAP)
    {
        ai_puts("[AI]   scause = ");
        ai_put_hex(record->scause);
        ai_puts("  sepc = ");
        ai_put_hex(record->sepc);
        ai_puts("  stval = ");
        ai_put_hex(record->stval);
        ai_putc('\n');
    }

    if (record->driver[0])
    {
        ai_puts("[AI]   inside driver: ");
        ai_puts(record->driver);
        ai_putc('\n');
    }
}

static void ai_snapshot_kernel(void)
{
    ai_image_size = (uint64_t)(kernel_image_end - kernel_start);

    if (ai_image_size > (uint64_t)(aispace_snapshot_end - aispace_snapshot))
    {
        ai_have_snapshot = 0;
        return;
    }

    ai_copy(aispace_snapshot, kernel_start, ai_image_size);
    ai_have_snapshot = 1;
}

static uint64_t ai_code_damage(void)
{
    uint64_t code_size = (uint64_t)(kernel_code_end - kernel_start);
    const volatile uint8_t *running = (const volatile uint8_t *)kernel_start;
    const volatile uint8_t *clean = (const volatile uint8_t *)aispace_snapshot;
    uint64_t damage = 0;

    for (uint64_t i = 0; i < code_size; i++)
    {
        if (running[i] != clean[i])
        {
            damage++;
        }
    }

    return damage;
}

static void ai_publish_disabled(void)
{
    volatile guardian_mailbox_t *mailbox = &guardian_mailbox;

    for (int i = 0; i < MAILBOX_DISABLED_MAX; i++)
    {
        for (int j = 0; j < MAILBOX_NAME_MAX; j++)
        {
            mailbox->disabled_drivers[i][j] = ai_disabled[i][j];
        }
    }
}

static void ai_disable_driver(const char *name)
{
    for (int i = 0; i < MAILBOX_DISABLED_MAX; i++)
    {
        if (ai_disabled[i][0] && ai_text_equal(ai_disabled[i], name))
        {
            return;
        }
    }

    for (int i = 0; i < MAILBOX_DISABLED_MAX; i++)
    {
        if (!ai_disabled[i][0])
        {
            ai_copy_text(ai_disabled[i], name, MAILBOX_NAME_MAX);
            ai_publish_disabled();
            return;
        }
    }
}

static void ai_mailbox_reset(uint32_t safe_mode)
{
    volatile guardian_mailbox_t *mailbox = &guardian_mailbox;
    uint64_t skip = MAILBOX_BOOT_ACK + sizeof(mailbox->boot_ack);

    /* Never clear boot_request/boot_ack: core 0 may be writing them */
    ai_zero((uint8_t *)mailbox + skip, sizeof(*mailbox) - skip);

    mailbox->ai_start_time = ai_start_time;
    mailbox->kernel_restarts = ai_restarts;
    mailbox->restart_safe_mode = safe_mode;
    ai_publish_disabled();

    __sync_synchronize();

    mailbox->aispace_magic = MAILBOX_MAGIC;

    __sync_synchronize();

    mailbox->aispace_state = AISPACE_STATE_ONLINE;
}

static void ai_serve_boot(void)
{
    volatile guardian_mailbox_t *mailbox = &guardian_mailbox;
    uint64_t request = mailbox->boot_request;

    if (request != mailbox->boot_ack)
    {
        __sync_synchronize();
        mailbox->boot_ack = request;
    }
}

static int ai_stop_kernel(void)
{
    volatile guardian_mailbox_t *mailbox = &guardian_mailbox;

    mailbox->core0_release = 0;
    mailbox->core0_parked = 0;

    __sync_synchronize();

    *(volatile uint32_t *)CLINT_MSIP_HART0 = 1;

    uint64_t start = ai_time();

    while (!mailbox->core0_parked)
    {
        if (ai_time() - start > AI_PARK_TIMEOUT)
        {
            *(volatile uint32_t *)CLINT_MSIP_HART0 = 0;
            return -1;
        }
    }

    return 0;
}

static void ai_restart_kernel(uint32_t safe_mode)
{
    ai_copy(kernel_start, aispace_snapshot, ai_image_size);
    ai_restarts++;

    ai_mailbox_reset(safe_mode);

    __sync_synchronize();

    guardian_mailbox.core0_release = 1;
}

/* Runs on core 0 in M-mode, entered from machine_trap when the AI space
   raises a machine software interrupt. It lives in the AI space so the
   kernel's own (maybe corrupted) code is not needed to stop core 0. */
void aispace_park_core0(void)
{
    volatile guardian_mailbox_t *mailbox = &guardian_mailbox;

    *(volatile uint32_t *)CLINT_MSIP_HART0 = 0;

    __sync_synchronize();

    mailbox->core0_parked = 1;

    while (!mailbox->core0_release)
    {
    }

    __sync_synchronize();

    asm volatile("fence.i");

    /* _start expects the device tree address in a1, like at power-on */
    register uintptr_t a1 asm("a1") = ai_dtb;
    asm volatile("jr %0" :: "r"(_start), "r"(a1));

    while (1)
    {
    }
}

static uint32_t ai_choose_action(uint64_t consecutive, int can_restart)
{
    if (consecutive > BLACKBOX_SAFE_MODE_THRESHOLD)
    {
        return BLACKBOX_ACTION_HALT;
    }

    if (consecutive == BLACKBOX_SAFE_MODE_THRESHOLD)
    {
        return can_restart ? BLACKBOX_ACTION_RESTART_SAFE : BLACKBOX_ACTION_SAFE_MODE;
    }

    return can_restart ? BLACKBOX_ACTION_RESTART : BLACKBOX_ACTION_REBOOT;
}

static void ai_handle(uint32_t crash_type)
{
    guardian_mailbox.aispace_state = AISPACE_STATE_HANDLING;

    ai_collect(crash_type);
    ai_print_report(&ai_record);

    int stopped = ai_stop_kernel() == 0;

    if (stopped)
    {
        ai_log("Core 0 stopped: the kernel is paused while the AI works");
    }
    else
    {
        ai_log("Core 0 did not stop: a warm restart is not possible");
    }

    uint64_t damage = 0;

    if (ai_have_snapshot)
    {
        damage = ai_code_damage();

        if (damage == 0)
        {
            ai_log("Kernel code check: intact (matches the clean copy)");
        }
        else
        {
            ai_puts("[AI] Kernel code check: ");
            ai_put_uint(damage);
            ai_puts(" bytes differ from the clean copy (code corrupted)\n");
        }
    }

    ai_diagnose(&ai_record);

    if (damage != 0)
    {
        ai_record.diagnosis[0] = 0;
        ai_append(ai_record.diagnosis,
                  "Kernel code was overwritten in memory: a bad pointer wrote over it. The clean copy fixes it.",
                  BLACKBOX_DIAGNOSIS_MAX);
    }

    ai_puts("[AI] Diagnosis: ");
    ai_puts(ai_record.diagnosis);
    ai_putc('\n');

    if (ai_record.driver[0])
    {
        ai_disable_driver(ai_record.driver);
        ai_puts("[AI] The crash happened inside driver ");
        ai_puts(ai_record.driver);
        ai_puts(": it will stay disabled\n");
    }

    ai_consecutive++;

    uint64_t consecutive = ai_consecutive;
    int saved = 0;

    blackbox_header_t header;

    if (ai_disk_init() == 0 &&
        ai_disk_read(blackbox_header_sector(ai_disk_capacity), ai_sector) == 0)
    {
        ai_copy(&header, ai_sector, sizeof(header));

        if (header.magic != BLACKBOX_MAGIC)
        {
            ai_zero(&header, sizeof(header));
            header.magic = BLACKBOX_MAGIC;
        }

        header.total_crashes++;
        header.consecutive_crashes++;
        consecutive = header.consecutive_crashes;
        ai_consecutive = consecutive;

        ai_record.sequence = header.total_crashes;
    }
    else
    {
        ai_zero(&header, sizeof(header));
    }

    ai_record.action = ai_choose_action(consecutive, stopped && ai_have_snapshot);

    if (header.magic == BLACKBOX_MAGIC)
    {
        uint8_t block[BLACKBOX_SECTOR_SIZE];

        ai_zero(block, BLACKBOX_SECTOR_SIZE);
        ai_copy(block, &ai_record, sizeof(ai_record));

        if (ai_disk_write(blackbox_record_sector(ai_disk_capacity, ai_record.sequence), block) == 0)
        {
            ai_zero(block, BLACKBOX_SECTOR_SIZE);
            ai_copy(block, &header, sizeof(header));

            if (ai_disk_write(blackbox_header_sector(ai_disk_capacity), block) == 0)
            {
                saved = 1;
            }
        }
    }

    if (saved)
    {
        ai_puts("[AI] Black box saved (crash #");
        ai_put_uint(ai_record.sequence);
        ai_puts(", ");
        ai_put_uint(consecutive);
        ai_puts(" in a row)\n");
    }
    else
    {
        ai_log("Black box NOT saved: disk unavailable");
    }

    ai_puts("[AI] Action: ");
    ai_puts(ai_action_name(ai_record.action));
    ai_putc('\n');

    if (ai_record.action == BLACKBOX_ACTION_HALT)
    {
        ai_log("Too many crashes in a row. The kernel stays stopped so the problem can be investigated.");
        ai_log("The AI space stays online. Reset the crash streak with: make reset-disk");

        ai_kernel_halted = 1;
        guardian_mailbox.aispace_state = AISPACE_STATE_ONLINE;
        return;
    }

    if (ai_record.action == BLACKBOX_ACTION_RESTART ||
        ai_record.action == BLACKBOX_ACTION_RESTART_SAFE)
    {
        ai_puts("[AI] Restoring the kernel from its clean copy (");
        ai_put_uint(ai_image_size / 1024);
        ai_puts(" KiB) and restarting core 0, restart #");
        ai_put_uint(ai_restarts + 1);
        ai_putc('\n');

        ai_restart_kernel(ai_record.action == BLACKBOX_ACTION_RESTART_SAFE);
        return;
    }

    ai_wait(AI_REBOOT_DELAY);

    *(volatile uint32_t *)AI_POWER = AI_POWER_REBOOT;

    while (1)
    {
    }
}

static void ai_handle_process_fault(void)
{
    volatile guardian_mailbox_t *mailbox = &guardian_mailbox;
    blackbox_record_t *record = &ai_fault_record;
    uint64_t sequence = mailbox->fault_seq;

    __sync_synchronize();

    ai_zero(record, sizeof(*record));

    record->crash_type = CRASH_TYPE_TRAP;
    record->pid = mailbox->fault_pid;
    record->scause = mailbox->fault_scause;
    record->sepc = mailbox->fault_sepc;
    record->stval = mailbox->fault_stval;
    ai_copy_text(record->process_name, mailbox->fault_name, BLACKBOX_NAME_MAX);
    ai_copy_text(record->driver, mailbox->fault_driver, BLACKBOX_NAME_MAX);

    uint32_t restarts = mailbox->fault_restarts;
    uint32_t user = mailbox->fault_user;
    uint32_t denied = mailbox->fault_denied;
    uint32_t trace_count = mailbox->fault_trace_count;

    if (user)
    {
        ai_diagnose_user(record);
    }
    else
    {
        ai_diagnose(record);
    }

    ai_puts("[AI] Process crash contained: ");
    ai_puts(record->process_name);
    ai_puts(" (pid ");
    ai_put_uint((uint64_t)record->pid);
    ai_puts(user ? ", user program" : ", kernel process");
    ai_puts("), the kernel keeps running\n");

    ai_puts("[AI]   scause = ");
    ai_put_hex(record->scause);
    ai_puts("  sepc = ");
    ai_put_hex(record->sepc);
    ai_puts("  stval = ");
    ai_put_hex(record->stval);
    ai_putc('\n');

    if (user)
    {
        ai_puts("[AI]   last system calls:");

        if (trace_count == 0)
        {
            ai_puts(" none");
        }

        for (uint32_t i = 0; i < trace_count && i < MAILBOX_TRACE_MAX; i++)
        {
            ai_puts(i == 0 ? " " : ", ");
            ai_puts(ai_syscall_name(mailbox->fault_trace[i]));
        }

        ai_puts("  (forbidden: ");
        ai_put_uint(denied);
        ai_puts(")\n");
    }

    ai_puts("[AI] Diagnosis: ");
    ai_puts(record->diagnosis);
    ai_putc('\n');

    if (denied > 0)
    {
        ai_puts("[AI] Security: it made ");
        ai_put_uint(denied);
        ai_puts(" forbidden system call(s) before crashing: treated as suspicious\n");
    }

    uint32_t action = 0;

    ai_puts("[AI] Action:");

    if (record->driver[0])
    {
        action |= VERDICT_DISABLE_DRIVER;
        ai_disable_driver(record->driver);

        ai_puts(" disable driver ");
        ai_puts(record->driver);
        ai_puts(" (the crash happened inside it),");
    }

    if (denied > 0)
    {
        ai_puts(" leave ");
        ai_puts(record->process_name);
        ai_puts(" stopped (suspicious program, not restarted)\n");
    }
    else if (restarts < AI_PROCESS_RESTART_LIMIT)
    {
        action |= VERDICT_RESTART_PROCESS;

        ai_puts(" restart ");
        ai_puts(record->process_name);
        ai_putc('\n');
    }
    else
    {
        ai_puts(" leave ");
        ai_puts(record->process_name);
        ai_puts(" stopped (it crashed ");
        ai_put_uint(restarts + 1);
        ai_puts(" times)\n");
    }

    mailbox->verdict_action = action;

    __sync_synchronize();

    mailbox->verdict_seq = sequence;
}

static void ai_trap(void) __attribute__((aligned(4)));

static void ai_trap(void)
{
    uint64_t mcause;
    uint64_t mepc;

    asm volatile("csrr %0, mcause" : "=r"(mcause));
    asm volatile("csrr %0, mepc" : "=r"(mepc));

    ai_puts("[AI] AI space fault: mcause = ");
    ai_put_hex(mcause);
    ai_puts(" mepc = ");
    ai_put_hex(mepc);
    ai_putc('\n');

    while (1)
    {
        asm volatile("wfi");
    }
}

void aispace_main(uintptr_t dtb)
{
    volatile guardian_mailbox_t *mailbox = &guardian_mailbox;

    asm volatile("csrw mtvec, %0" :: "r"((uint64_t)(uintptr_t)ai_trap));

    ai_start_time = ai_time();
    ai_dtb = dtb;

    /* Core 0 waits in boot.S until we acknowledge it, so the kernel
       has not changed its own memory yet: this copy is clean */
    ai_snapshot_kernel();
    ai_mailbox_reset(0);

    uint64_t last_beat = mailbox->heartbeat;
    uint64_t last_change = ai_time();

    while (1)
    {
        ai_serve_boot();

        if (ai_kernel_halted)
        {
            ai_wait(AI_POLL_INTERVAL);
            continue;
        }

        if (mailbox->streak_reset)
        {
            mailbox->streak_reset = 0;
            ai_consecutive = 0;
        }

        uint32_t crash_type = 0;

        if (mailbox->kernel_state == KERNEL_STATE_PANICKED)
        {
            __sync_synchronize();
            crash_type = mailbox->crash_type;
        }
        else if (mailbox->fault_seq != mailbox->verdict_seq)
        {
            ai_handle_process_fault();
        }

        uint64_t beat = mailbox->heartbeat;
        uint64_t now = ai_time();

        if (beat != last_beat)
        {
            last_beat = beat;
            last_change = now;
        }
        else if (crash_type == 0 &&
                 mailbox->watch_enabled &&
                 now - last_change > AI_FREEZE_TIMEOUT)
        {
            crash_type = CRASH_TYPE_FREEZE;
        }

        if (crash_type != 0)
        {
            ai_handle(crash_type);

            last_beat = mailbox->heartbeat;
            last_change = ai_time();
            continue;
        }

        ai_wait(AI_POLL_INTERVAL);
    }
}

#include "process.h"
#include "heap.h"
#include "timer.h"
#include "trap.h"
#include "uart.h"
#include "logging.h"
#include "guardian.h"
#include "vm.h"
#include "page.h"
#include "elf.h"
#include "program.h"
#include "string.h"
#include "syscall_abi.h"
#include "spinlock.h"
#include "knocfs.h"
#include "tty.h"

#define VRUNTIME_SCALE 600

typedef struct process
{
    int pid;
    char name[PROCESS_NAME_MAX];
    process_class_t process_class;
    process_state_t state;
    process_context_t context;
    void *stack;
    process_entry_t entry;
    void *arg;
    uint64_t cpu_ticks;
    uint64_t vruntime;
    uint64_t slice_left;
    uint64_t wake_tick;
    const char *driver;
    uint32_t restarts;
    int fault_reported;
    const char *fault_driver;
    uint64_t fault_scause;
    uint64_t fault_sepc;
    uint64_t fault_stval;
    uint64_t satp;
    int exit_code;

    /* User programs only */
    int user;
    const program_t *program;
    uintptr_t user_root;
    uintptr_t user_entry;
    uint32_t capabilities;
    uint64_t mem_limit;
    uint64_t mem_used;
    uintptr_t heap_next;
    void *blocks[PROCESS_BLOCKS_MAX];
    int block_count;
    uint8_t trace[PROCESS_TRACE_MAX];
    uint32_t trace_count;
    uint32_t denied;

    /* Wait queues */
    void *wait_channel;
    int woken;
    sleeplock_t *locks[PROCESS_LOCKS_MAX];

    open_file_t files[PROCESS_FILES_MAX];

    uint64_t fp_state[33];
    char args[ARGS_MAX];
} process_t;

typedef struct class_info
{
    const char *name;
    uint64_t weight;
    uint64_t slice;
} class_info_t;

static const class_info_t class_info[] = {
    [PROCESS_CLASS_INTERACTIVE] = {"INTERACTIVE", 0, SCHED_SLICE_INTERACTIVE},
    [PROCESS_CLASS_AI_AGENT] = {"AI_AGENT", SCHED_WEIGHT_AI_AGENT, SCHED_SLICE_AI_AGENT},
    [PROCESS_CLASS_NORMAL] = {"NORMAL", SCHED_WEIGHT_NORMAL, SCHED_SLICE_NORMAL},
    [PROCESS_CLASS_BACKGROUND] = {"BACKGROUND", SCHED_WEIGHT_BACKGROUND, SCHED_SLICE_BACKGROUND},
    [PROCESS_CLASS_IDLE] = {"IDLE", 0, 1},
};

static const char *state_names[] = {
    [PROCESS_UNUSED] = "UNUSED",
    [PROCESS_READY] = "READY",
    [PROCESS_RUNNING] = "RUNNING",
    [PROCESS_SLEEPING] = "SLEEPING",
    [PROCESS_EXITED] = "EXITED",
    [PROCESS_CRASHED] = "CRASHED",
    [PROCESS_LOADING] = "LOADING",
    [PROCESS_BLOCKED] = "BLOCKED",
};

static process_t processes[PROCESS_MAX];
static process_t *current = 0;
static int next_pid = 1;
static int scheduler_running = 0;
static int resched_pending = 0;
static uint64_t block_count = 0;
static char crash_channel;
static uint64_t disk_loads = 0;

#define PROGRAM_FILE_MAX (16UL * 1024 * 1024)
static const char *boot_driver = 0;

extern void context_switch(process_context_t *old_context,
                           process_context_t *new_context);
extern void fp_save(uint64_t *state);
extern void fp_restore(uint64_t *state);

extern void user_enter(uintptr_t entry, uintptr_t user_sp, uintptr_t kernel_sp)
    __attribute__((noreturn));

static uint64_t interrupts_disable(void)
{
    uint64_t previous;
    asm volatile("csrrc %0, sstatus, %1"
                 : "=r"(previous)
                 : "r"((uint64_t)SSTATUS_SIE));
    return previous & SSTATUS_SIE;
}

static void interrupts_restore(uint64_t enabled)
{
    if (enabled)
    {
        asm volatile("csrs sstatus, %0" :: "r"((uint64_t)SSTATUS_SIE));
    }
}

static int is_weighted(process_t *p)
{
    return class_info[p->process_class].weight != 0;
}

static void copy_name(char *destination, const char *source)
{
    int i = 0;

    while (source[i] && i < PROCESS_NAME_MAX - 1)
    {
        destination[i] = source[i];
        i++;
    }

    destination[i] = 0;
}

static void process_trampoline(void);

static void reset_context(process_t *p)
{
    process_context_t empty = {0};

    for (int i = 0; i < 33; i++)
    {
        p->fp_state[i] = 0;
    }

    p->context = empty;
    p->context.ra = (uint64_t)(uintptr_t)process_trampoline;
    p->context.sp = ((uintptr_t)p->stack + PROCESS_STACK_SIZE) & ~0xFUL;
}

static int lowest_vruntime(process_t *except, uint64_t *result)
{
    int found = 0;

    for (int i = 0; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if (p == except || !is_weighted(p))
        {
            continue;
        }

        if (p->state != PROCESS_READY && p->state != PROCESS_RUNNING)
        {
            continue;
        }

        if (!found || p->vruntime < *result)
        {
            *result = p->vruntime;
            found = 1;
        }
    }

    return found;
}

static void place_vruntime(process_t *p)
{
    uint64_t lowest;

    if (is_weighted(p) && lowest_vruntime(p, &lowest) && p->vruntime < lowest)
    {
        p->vruntime = lowest;
    }
}

static process_t *pick_next(void)
{
    int start = (int)(current - processes);

    for (int i = 1; i <= PROCESS_MAX; i++)
    {
        process_t *p = &processes[(start + i) % PROCESS_MAX];

        if (p->state == PROCESS_READY &&
            p->process_class == PROCESS_CLASS_INTERACTIVE)
        {
            return p;
        }
    }

    process_t *best = 0;

    for (int i = 0; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if (p->state != PROCESS_READY || !is_weighted(p))
        {
            continue;
        }

        if (best == 0 ||
            p->vruntime < best->vruntime ||
            (p->vruntime == best->vruntime &&
             p->process_class < best->process_class))
        {
            best = p;
        }
    }

    if (best != 0)
    {
        return best;
    }

    return &processes[0];
}

static void schedule(void)
{
    process_t *previous = current;

    if (previous->state == PROCESS_RUNNING)
    {
        previous->state = PROCESS_READY;
    }

    process_t *next = pick_next();

    next->state = PROCESS_RUNNING;
    next->slice_left = class_info[next->process_class].slice;

    if (next == previous)
    {
        return;
    }

    current = next;
    guardian_set_current(next->pid, next->name);
    vm_switch(next->satp);
    fp_save(previous->fp_state);
    fp_restore(next->fp_state);
    context_switch(&previous->context, &next->context);
}

static void process_trampoline(void)
{
    interrupts_restore(1);

    current->entry(current->arg);

    process_exit();
}

void process_init(void)
{
    process_t *idle = &processes[0];

    idle->pid = 0;
    copy_name(idle->name, "idle");
    idle->process_class = PROCESS_CLASS_IDLE;
    idle->state = PROCESS_RUNNING;
    idle->stack = 0;
    idle->slice_left = class_info[PROCESS_CLASS_IDLE].slice;

    idle->satp = vm_kernel_satp();

    current = idle;
    guardian_set_current(idle->pid, idle->name);
}

/* Takes a free slot and sets up a READY kernel thread. Interrupts must be off. */
static process_t *create_locked(const char *name,
                                process_class_t process_class,
                                process_entry_t entry,
                                void *arg)
{
    process_t *p = 0;

    for (int i = 1; i < PROCESS_MAX; i++)
    {
        process_t *candidate = &processes[i];

        if (candidate == current)
        {
            continue;
        }

        if (candidate->state == PROCESS_UNUSED ||
            candidate->state == PROCESS_EXITED)
        {
            p = candidate;
            break;
        }
    }

    if (p == 0 || process_class == PROCESS_CLASS_IDLE)
    {
        return 0;
    }

    if (p->stack != 0)
    {
        kfree(p->stack);
        p->stack = 0;
    }

    void *stack = kmalloc(PROCESS_STACK_SIZE);

    if (stack == 0)
    {
        p->state = PROCESS_UNUSED;
        return 0;
    }

    p->pid = next_pid++;
    copy_name(p->name, name);
    p->process_class = process_class;
    p->stack = stack;
    p->entry = entry;
    p->arg = arg;
    p->cpu_ticks = 0;
    p->vruntime = 0;
    p->slice_left = 0;
    p->wake_tick = 0;
    p->driver = 0;
    p->restarts = 0;
    p->fault_reported = 0;
    p->fault_driver = 0;
    p->satp = vm_kernel_satp();
    p->exit_code = 0;
    p->user = 0;
    p->program = 0;
    p->user_root = 0;
    p->block_count = 0;
    p->mem_used = 0;
    p->trace_count = 0;
    p->denied = 0;
    p->wait_channel = 0;
    p->woken = 0;
    p->args[0] = 0;

    for (int i = 0; i < PROCESS_LOCKS_MAX; i++)
    {
        p->locks[i] = 0;
    }

    for (int i = 0; i < PROCESS_FILES_MAX; i++)
    {
        p->files[i].used = 0;
    }

    reset_context(p);
    p->state = PROCESS_READY;

    place_vruntime(p);

    return p;
}

int process_create(const char *name,
                   process_class_t process_class,
                   process_entry_t entry,
                   void *arg)
{
    uint64_t enabled = interrupts_disable();
    process_t *p = create_locked(name, process_class, entry, arg);
    int pid = p != 0 ? p->pid : -1;

    interrupts_restore(enabled);
    return pid;
}

/* ---- User programs ---- */

static uint64_t block_size(uint64_t bytes)
{
    uint64_t size = PAGE_SIZE;

    while (size < bytes)
    {
        size <<= 1;
    }

    return size;
}

/* Zeroed memory for a user program, counted against its quota and
   remembered so it can be freed when the program ends */
static void *user_block(void *context, uint64_t bytes)
{
    process_t *p = (process_t *)context;
    uint64_t size = block_size(bytes);

    if (p->block_count >= PROCESS_BLOCKS_MAX || p->mem_used + size > p->mem_limit)
    {
        return 0;
    }

    void *memory = page_alloc_contiguous(size);

    if (memory == 0)
    {
        return 0;
    }

    memset(memory, 0, size);

    p->blocks[p->block_count++] = memory;
    p->mem_used += size;

    return memory;
}

static void user_space_free(process_t *p)
{
    if (p->user_root != 0)
    {
        vm_user_destroy(p->user_root);
        p->user_root = 0;
    }

    for (int i = 0; i < p->block_count; i++)
    {
        page_free(p->blocks[i]);
    }

    p->block_count = 0;
    p->mem_used = 0;
    p->satp = vm_kernel_satp();
}

/* Programs load from /bin/<name> on disk. The copy built into the
   kernel is the fallback: no disk, no filesystem, or disk0 disabled */
static void *load_from_disk(const program_t *program, uint64_t *size)
{
    char path[PROCESS_NAME_MAX + 8] = "/bin/";
    uint32_t inode;
    knocfs_stat_t stat;
    int length = 5;

    for (int i = 0; program->name[i] && length < (int)sizeof(path) - 1; i++)
    {
        path[length++] = program->name[i];
    }

    path[length] = 0;

    if (!knocfs_mounted() ||
        knocfs_lookup(path, &inode) != 0 ||
        knocfs_stat(inode, &stat) != 0 ||
        stat.type != KNOCFS_TYPE_FILE ||
        stat.size == 0 || stat.size > PROGRAM_FILE_MAX)
    {
        return 0;
    }

    void *buffer = page_alloc_contiguous(stat.size);

    if (buffer == 0)
    {
        return 0;
    }

    if (knocfs_read(inode, 0, buffer, stat.size) != (int64_t)stat.size)
    {
        page_free(buffer);
        return 0;
    }

    *size = stat.size;
    return buffer;
}

static int user_space_load(process_t *p)
{
    const program_t *program = p->program;
    uint64_t size = (uint64_t)(program->end - program->start);
    void *file = load_from_disk(program, &size);
    const uint8_t *image = file != 0 ? (const uint8_t *)file : program->start;

    for (int i = 0; i < PROCESS_FILES_MAX; i++)
    {
        p->files[i].used = 0;
    }

    p->user_root = vm_user_create();
    p->block_count = 0;
    p->mem_used = 0;
    p->heap_next = USER_HEAP_BASE;
    p->trace_count = 0;
    p->denied = 0;

    int loaded = p->user_root != 0 &&
                 elf_load(p->user_root, image, size, user_block, p, &p->user_entry) == 0;

    if (file != 0)
    {
        page_free(file);
        disk_loads += loaded;
    }

    if (!loaded)
    {
        user_space_free(p);
        return -1;
    }

    void *stack = user_block(p, USER_STACK_SIZE);

    if (stack == 0 ||
        vm_user_map(p->user_root,
                    USER_STACK_TOP - USER_STACK_SIZE,
                    (uintptr_t)stack,
                    USER_STACK_SIZE,
                    PTE_R | PTE_W) != 0)
    {
        user_space_free(p);
        return -1;
    }

    p->satp = vm_make_satp(p->user_root);
    return 0;
}

/* The first code a user process runs, still in the kernel: drop to U-mode */
static void user_process_start(void *arg)
{
    (void)arg;

    interrupts_disable();

    uintptr_t kernel_sp = ((uintptr_t)current->stack + PROCESS_STACK_SIZE) & ~0xFUL;

    user_enter(current->user_entry, USER_STACK_TOP, kernel_sp);
}

int process_spawn(const program_t *program)
{
    return process_spawn_args(program, "");
}

const char *process_args(void)
{
    return current->args;
}

int process_spawn_args(const program_t *program, const char *args)
{
    if (program == 0)
    {
        return -1;
    }

    uint64_t enabled = interrupts_disable();
    process_t *p = create_locked(program->name, program->process_class, user_process_start, 0);

    if (p == 0)
    {
        interrupts_restore(enabled);
        return -1;
    }

    /* Not READY yet: the scheduler must not run it before it's loaded */
    p->state = PROCESS_LOADING;
    p->user = 1;
    p->program = program;

    int length = 0;

    while (args[length] && length < ARGS_MAX - 1)
    {
        p->args[length] = args[length];
        length++;
    }

    p->args[length] = 0;
    p->capabilities = program->capabilities;
    p->mem_limit = program->process_class == PROCESS_CLASS_AI_AGENT
                       ? PROCESS_QUOTA_AI_AGENT
                       : PROCESS_QUOTA_DEFAULT;

    interrupts_restore(enabled);

    int loaded = user_space_load(p);

    enabled = interrupts_disable();

    if (loaded != 0)
    {
        p->user = 0;
        p->state = PROCESS_EXITED;
        interrupts_restore(enabled);
        return -1;
    }

    p->state = PROCESS_READY;
    place_vruntime(p);

    int pid = p->pid;

    if (program->flags & PROGRAM_TERMINAL)
    {
        tty_set_owner(pid);
    }

    interrupts_restore(enabled);
    return pid;
}

int process_wait(int pid, int *exit_code)
{
    while (1)
    {
        process_t *p = 0;

        for (int i = 1; i < PROCESS_MAX; i++)
        {
            if (processes[i].state != PROCESS_UNUSED && processes[i].pid == pid)
            {
                p = &processes[i];
            }
        }

        if (p == 0)
        {
            return -1;
        }

        if (p->state == PROCESS_CRASHED)
        {
            return -2;
        }

        if (p->state == PROCESS_EXITED)
        {
            *exit_code = p->exit_code;
            return 0;
        }

        uint64_t enabled = interrupts_disable();

        if (p->pid == pid && p->state != PROCESS_EXITED && p->state != PROCESS_CRASHED)
        {
            process_block(p, 0);
        }

        interrupts_restore(enabled);
    }
}

open_file_t *process_file(int fd)
{
    if (fd < FD_FIRST_FILE || fd >= FD_FIRST_FILE + PROCESS_FILES_MAX)
    {
        return 0;
    }

    open_file_t *file = &current->files[fd - FD_FIRST_FILE];

    return file->used ? file : 0;
}

int process_file_open(uint32_t inode, uint32_t flags)
{
    for (int i = 0; i < PROCESS_FILES_MAX; i++)
    {
        open_file_t *file = &current->files[i];

        if (!file->used)
        {
            file->used = 1;
            file->inode = inode;
            file->flags = flags;
            file->offset = 0;
            return FD_FIRST_FILE + i;
        }
    }

    return -1;
}

uint64_t process_disk_loads(void)
{
    return disk_loads;
}

int process_is_user(void)
{
    return current != 0 && current->user;
}

uintptr_t process_user_root(void)
{
    return current->user_root;
}

uint32_t process_capabilities(void)
{
    return current->capabilities;
}

void process_record_syscall(uint64_t number)
{
    current->trace[current->trace_count % PROCESS_TRACE_MAX] =
        number < 255 ? (uint8_t)number : 255;
    current->trace_count++;
}

void process_note_denied(void)
{
    current->denied++;
}

int64_t process_mem_alloc(uint64_t bytes)
{
    if (bytes == 0 || bytes > PROCESS_QUOTA_AI_AGENT)
    {
        return E_INVAL;
    }

    uint64_t size = block_size(bytes);
    uintptr_t alignment = size >= VM_MEGAPAGE_SIZE ? VM_MEGAPAGE_SIZE : PAGE_SIZE;
    uintptr_t address = (current->heap_next + alignment - 1) & ~(alignment - 1);

    if (address + size > USER_HEAP_END)
    {
        return E_NOMEM;
    }

    void *memory = user_block(current, size);

    if (memory == 0)
    {
        return E_NOMEM;
    }

    if (vm_user_map(current->user_root, address, (uintptr_t)memory, size, PTE_R | PTE_W) != 0)
    {
        return E_NOMEM;
    }

    asm volatile("sfence.vma zero, zero");

    current->heap_next = address + size;
    return (int64_t)address;
}

void scheduler_start(void)
{
    scheduler_running = 1;
    log_info("Scheduler started (AI-aware: INTERACTIVE first, then AI_AGENT 60 / NORMAL 30 / BACKGROUND 10)");
}

void scheduler_tick(void)
{
    if (!scheduler_running)
    {
        return;
    }

    uint64_t now = timer_ticks();

    current->cpu_ticks++;

    if (is_weighted(current))
    {
        current->vruntime += VRUNTIME_SCALE / class_info[current->process_class].weight;
    }

    int interactive_woke = 0;

    for (int i = 0; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if ((p->state == PROCESS_SLEEPING && now >= p->wake_tick) ||
            (p->state == PROCESS_BLOCKED && p->wake_tick != 0 && now >= p->wake_tick))
        {
            p->state = PROCESS_READY;
            place_vruntime(p);

            if (p->process_class == PROCESS_CLASS_INTERACTIVE)
            {
                interactive_woke = 1;
            }
        }
    }

    if (current->slice_left > 0)
    {
        current->slice_left--;
    }

    int need_switch = current->slice_left == 0;

    if (interactive_woke && current->process_class != PROCESS_CLASS_INTERACTIVE)
    {
        need_switch = 1;
    }

    if (current == &processes[0])
    {
        need_switch = 1;
    }

    if (need_switch)
    {
        schedule();
    }
}

int process_can_block(void)
{
    return scheduler_running && current != 0 && current != &processes[0];
}

int process_block(void *channel, uint64_t timeout)
{
    uint64_t enabled = interrupts_disable();

    current->wait_channel = channel;
    current->wake_tick = timeout != 0 ? timer_ticks() + timeout : 0;
    current->woken = 0;
    current->state = PROCESS_BLOCKED;
    block_count++;

    schedule();

    current->wait_channel = 0;

    interrupts_restore(enabled);
    return current->woken;
}

void process_wake(void *channel)
{
    uint64_t enabled = interrupts_disable();

    for (int i = 0; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if (p->state != PROCESS_BLOCKED || p->wait_channel != channel)
        {
            continue;
        }

        p->woken = 1;
        p->state = PROCESS_READY;
        place_vruntime(p);

        /* Run it right away if it's interactive, or if the CPU is idle */
        if (current == &processes[0] ||
            (p->process_class == PROCESS_CLASS_INTERACTIVE &&
             current->process_class != PROCESS_CLASS_INTERACTIVE))
        {
            resched_pending = 1;
        }
    }

    interrupts_restore(enabled);
}

/* Called at the end of a device interrupt: switch now if a wake-up asked for it */
void scheduler_preempt(void)
{
    if (scheduler_running && resched_pending)
    {
        resched_pending = 0;
        schedule();
    }
}

uint64_t process_block_count(void)
{
    return block_count;
}

static int unreported_crash(void)
{
    for (int i = 1; i < PROCESS_MAX; i++)
    {
        if (processes[i].state == PROCESS_CRASHED && !processes[i].fault_reported)
        {
            return 1;
        }
    }

    return 0;
}

void process_wait_crash(uint64_t timeout)
{
    uint64_t enabled = interrupts_disable();

    if (!unreported_crash())
    {
        process_block(&crash_channel, timeout);
    }

    interrupts_restore(enabled);
}

void sleeplock_acquire(sleeplock_t *lock)
{
    uint64_t enabled = interrupts_disable();

    while (lock->locked)
    {
        if (process_can_block())
        {
            process_block(lock, 0);
        }
        else
        {
            interrupts_restore(enabled);
            asm volatile("wfi");
            enabled = interrupts_disable();
        }
    }

    lock->locked = 1;
    lock->owner = current != 0 ? current->pid : 0;

    if (current != 0)
    {
        for (int i = 0; i < PROCESS_LOCKS_MAX; i++)
        {
            if (current->locks[i] == 0)
            {
                current->locks[i] = lock;
                break;
            }
        }
    }

    interrupts_restore(enabled);
}

void sleeplock_release(sleeplock_t *lock)
{
    uint64_t enabled = interrupts_disable();

    lock->locked = 0;
    lock->owner = 0;

    if (current != 0)
    {
        for (int i = 0; i < PROCESS_LOCKS_MAX; i++)
        {
            if (current->locks[i] == lock)
            {
                current->locks[i] = 0;
            }
        }
    }

    process_wake(lock);

    interrupts_restore(enabled);
}

void process_yield(void)
{
    uint64_t enabled = interrupts_disable();
    schedule();
    interrupts_restore(enabled);
}

void process_sleep(uint64_t ticks)
{
    uint64_t enabled = interrupts_disable();

    current->wake_tick = timer_ticks() + ticks;
    current->state = PROCESS_SLEEPING;
    schedule();

    interrupts_restore(enabled);
}

void process_exit_code(int code)
{
    interrupts_disable();

    current->exit_code = code;

    if (current->user)
    {
        /* Leave the program's page table before freeing it */
        vm_switch(vm_kernel_satp());
        user_space_free(current);
    }

    current->state = PROCESS_EXITED;
    process_wake(current);
    schedule();

    while (1)
    {
    }
}

void process_exit(void)
{
    process_exit_code(0);
}

static void release_locks(process_t *p)
{
    for (int i = 0; i < PROCESS_LOCKS_MAX; i++)
    {
        if (p->locks[i] != 0)
        {
            p->locks[i]->locked = 0;
            p->locks[i]->owner = 0;
            process_wake(p->locks[i]);
            p->locks[i] = 0;
        }
    }
}

static int kill_locked(process_t *p)
{
    if (p->user)
    {
        user_space_free(p);
    }

    release_locks(p);

    p->exit_code = E_KILLED;
    p->state = PROCESS_EXITED;
    process_wake(p);
    return 0;
}

static process_t *find_live(int pid)
{
    for (int i = 1; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if (p->state != PROCESS_UNUSED &&
            p->state != PROCESS_EXITED &&
            p->pid == pid)
        {
            return p;
        }
    }

    return 0;
}

int process_kill(int pid)
{
    uint64_t enabled = interrupts_disable();
    process_t *p = find_live(pid);
    int result = -1;

    if (p != 0 && p != current && p->state != PROCESS_LOADING)
    {
        result = kill_locked(p);
    }

    interrupts_restore(enabled);
    return result;
}

/* From the shell: only user programs can be killed, never the kernel's own processes */
int process_kill_user(int pid)
{
    uint64_t enabled = interrupts_disable();
    process_t *p = find_live(pid);
    int result = E_NOTFOUND;

    if (p != 0)
    {
        result = (!p->user || p == current || p->state == PROCESS_LOADING) ? E_PERM : kill_locked(p);
    }

    interrupts_restore(enabled);
    return result;
}

int process_alive(int pid)
{
    uint64_t enabled = interrupts_disable();
    process_t *p = find_live(pid);
    int alive = p != 0 && p->state != PROCESS_CRASHED;

    interrupts_restore(enabled);
    return alive;
}

int process_info(uint32_t index, process_info_t *info)
{
    uint64_t enabled = interrupts_disable();
    uint32_t seen = 0;

    for (int i = 0; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if (p->state == PROCESS_UNUSED || p->state == PROCESS_EXITED)
        {
            continue;
        }

        if (seen++ != index)
        {
            continue;
        }

        info->pid = p->pid;
        info->process_class = p->process_class;
        info->state = p->state;
        info->user = (uint32_t)p->user;
        info->restarts = p->restarts;
        info->reserved = 0;
        info->cpu_ticks = p->cpu_ticks;
        info->memory = p->mem_used;
        memset(info->name, 0, sizeof(info->name));
        copy_name(info->name, p->name);

        interrupts_restore(enabled);
        return 0;
    }

    interrupts_restore(enabled);
    return -1;
}

int process_can_contain_fault(void)
{
    return scheduler_running && current != 0 && current != &processes[0];
}

void process_crash(uint64_t scause, uint64_t sepc, uint64_t stval)
{
    interrupts_disable();

    current->state = PROCESS_CRASHED;
    current->fault_reported = 0;

    /* A crashed process never releases its locks itself: do it now, so
       the disk (or anything else it held) doesn't stay locked forever */
    for (int i = 0; i < PROCESS_LOCKS_MAX; i++)
    {
        if (current->locks[i] != 0)
        {
            current->locks[i]->locked = 0;
            current->locks[i]->owner = 0;
            process_wake(current->locks[i]);
            current->locks[i] = 0;
        }
    }

    process_wake(current);
    process_wake(&crash_channel);
    current->fault_driver = current->driver;
    current->fault_scause = scause;
    current->fault_sepc = sepc;
    current->fault_stval = stval;
    current->driver = 0;

    schedule();

    while (1)
    {
    }
}

int process_next_crash(process_fault_t *fault)
{
    uint64_t enabled = interrupts_disable();

    for (int i = 1; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if (p->state != PROCESS_CRASHED || p->fault_reported)
        {
            continue;
        }

        p->fault_reported = 1;

        fault->pid = p->pid;
        copy_name(fault->name, p->name);
        copy_name(fault->driver, p->fault_driver ? p->fault_driver : "");
        fault->scause = p->fault_scause;
        fault->sepc = p->fault_sepc;
        fault->stval = p->fault_stval;
        fault->restarts = p->restarts;
        fault->user = p->user;
        fault->denied = p->denied;
        fault->trace_count = p->trace_count < PROCESS_TRACE_MAX ? p->trace_count : PROCESS_TRACE_MAX;

        for (uint32_t t = 0; t < fault->trace_count; t++)
        {
            uint32_t index = p->trace_count - fault->trace_count + t;
            fault->trace[t] = p->trace[index % PROCESS_TRACE_MAX];
        }

        interrupts_restore(enabled);
        return 0;
    }

    interrupts_restore(enabled);
    return -1;
}

static process_t *find_crashed(int pid)
{
    for (int i = 1; i < PROCESS_MAX; i++)
    {
        if (processes[i].state == PROCESS_CRASHED && processes[i].pid == pid)
        {
            return &processes[i];
        }
    }

    return 0;
}

int process_restart(int pid)
{
    uint64_t enabled = interrupts_disable();
    process_t *p = find_crashed(pid);

    if (p == 0)
    {
        interrupts_restore(enabled);
        return -1;
    }

    /* Keep the slot while the program is reloaded (it may sleep on the disk) */
    p->state = PROCESS_LOADING;
    interrupts_restore(enabled);

    if (p->user)
    {
        user_space_free(p);

        if (user_space_load(p) != 0)
        {
            enabled = interrupts_disable();
            p->user = 0;
            p->state = PROCESS_EXITED;
            interrupts_restore(enabled);
            return -1;
        }
    }

    enabled = interrupts_disable();

    p->pid = next_pid++;
    p->restarts++;
    p->cpu_ticks = 0;
    p->slice_left = 0;
    p->wake_tick = 0;
    p->driver = 0;
    p->fault_reported = 0;
    p->fault_driver = 0;
    reset_context(p);
    p->state = PROCESS_READY;

    place_vruntime(p);

    int new_pid = p->pid;

    if (p->user && (p->program->flags & PROGRAM_TERMINAL))
    {
        tty_set_owner(new_pid);
    }

    interrupts_restore(enabled);
    return new_pid;
}

void process_discard(int pid)
{
    uint64_t enabled = interrupts_disable();
    process_t *p = find_crashed(pid);

    if (p != 0)
    {
        if (p->user)
        {
            user_space_free(p);
        }

        p->state = PROCESS_EXITED;
    }

    interrupts_restore(enabled);
}

const char *process_driver_enter(const char *name)
{
    const char **slot = current != 0 ? &current->driver : &boot_driver;
    const char *previous = *slot;

    *slot = name;
    return previous;
}

void process_driver_leave(const char *previous)
{
    const char **slot = current != 0 ? &current->driver : &boot_driver;

    *slot = previous;
}

const char *process_current_driver(void)
{
    return current != 0 ? current->driver : boot_driver;
}

int process_current_pid(void)
{
    return current->pid;
}

const char *process_current_name(void)
{
    return current != 0 ? current->name : "kernel-boot";
}

uint64_t process_cpu_ticks(int pid)
{
    for (int i = 0; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if (p->state != PROCESS_UNUSED && p->pid == pid)
        {
            return p->cpu_ticks;
        }
    }

    return 0;
}

static void put_padded(const char *text, int width)
{
    int length = 0;

    while (text[length])
    {
        length++;
    }

    uart_puts(text);

    while (length < width)
    {
        uart_putc(' ');
        length++;
    }
}

static void put_padded_uint(uint64_t value, int width)
{
    char digits[21];
    int length = 0;

    do
    {
        digits[length++] = '0' + (value % 10);
        value /= 10;
    } while (value > 0);

    char text[21];

    for (int i = 0; i < length; i++)
    {
        text[i] = digits[length - 1 - i];
    }

    text[length] = 0;
    put_padded(text, width);
}

void process_list(void)
{
    uint64_t enabled = interrupts_disable();

    uart_puts("       PID  NAME            CLASS        STATE     CPU TICKS\n");

    for (int i = 0; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if (p->state == PROCESS_UNUSED)
        {
            continue;
        }

        uart_puts("       ");
        put_padded_uint((uint64_t)p->pid, 5);
        put_padded(p->name, 16);
        put_padded(class_info[p->process_class].name, 13);
        put_padded(state_names[p->state], 10);
        put_padded_uint(p->cpu_ticks, 0);
        uart_putc('\n');
    }

    interrupts_restore(enabled);
}

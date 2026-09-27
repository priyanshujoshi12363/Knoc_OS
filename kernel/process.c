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
#include "memgraph.h"
#include "net.h"
#include "cpu.h"
#include "aispace.h"
#include "linux.h"

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
    uint64_t fault_sp;
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
    char cwd[PATH_MAX];
    uint64_t syscalls;
    uint64_t seen_cpu;
    uint64_t seen_syscalls;
    uint64_t spawned;
    uint64_t seen_spawned;
    uint64_t disk_bytes;
    uint64_t seen_disk_bytes;
    int capture_owner;
    int capture_pid;
    int capture_quiet;
    uint32_t capture_length;
    char capture[PROCESS_CAPTURE_MAX];
    uint64_t block_seq;
    int cpu;
    uint32_t pin_mask;
    int kill_pending;
    struct process *leader;
    int space_pending;
    uintptr_t thread_stack;
    uintptr_t thread_argument;
    void *linux_state;
    uintptr_t user_sp;
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
static process_t idle_processes[CPU_MAX];

#define current (cpu_self()->running)
static int next_pid = 1;
static int scheduler_running = 0;
static uint64_t block_count = 0;
static char crash_channel;
static uint64_t disk_loads = 0;
static uint64_t count_switches;
static uint64_t count_syscalls;
static uint64_t count_denied;
static uint64_t count_spawns;
static uint64_t count_crashes;
static uint64_t seen_ticks;

#define PROGRAM_FILE_MAX (16UL * 1024 * 1024)
static const char *boot_driver = 0;

extern void context_switch(process_context_t *old_context,
                           process_context_t *new_context);
extern void fp_save(uint64_t *state);
extern void fp_restore(uint64_t *state);

extern void user_enter(uintptr_t entry, uintptr_t user_sp, uintptr_t kernel_sp, uintptr_t argument)
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

static int is_idle(process_t *p)
{
    return p->process_class == PROCESS_CLASS_IDLE;
}

static uint32_t allowed_cpus(process_t *p)
{
    uint32_t wanted = p->pin_mask;

    if (wanted == 0)
    {
        wanted = p->process_class == PROCESS_CLASS_AI_AGENT || p->process_class == PROCESS_CLASS_BACKGROUND
                     ? CPU_MASK_AI
                     : CPU_MASK_GENERAL;
    }

    uint32_t online = cpu_online_bits;

    if (wanted & online)
    {
        return wanted & online;
    }

    return (CPU_MASK_GENERAL & online) ? CPU_MASK_GENERAL & online : 1U;
}

static int runs_here(process_t *p)
{
    return (allowed_cpus(p) >> cpu_id()) & 1;
}

static int is_running_anywhere(process_t *p)
{
    for (int i = 0; i < CPU_MAX; i++)
    {
        if (cpus[i].running == p)
        {
            return 1;
        }
    }

    return 0;
}

static process_t *owner_of(process_t *p)
{
    return p->leader ? p->leader : p;
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

        if (p == except || !is_weighted(p) || !(allowed_cpus(p) & allowed_cpus(except)))
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

static void kick_for(process_t *p)
{
    uint32_t mask = allowed_cpus(p);
    int self = cpu_id();

    if (((mask >> self) & 1) && is_idle(current))
    {
        return;
    }

    for (int i = 0; i < CPU_MAX; i++)
    {
        if (i != self && ((mask >> i) & 1) && cpus[i].online && !cpus[i].kicked &&
            cpus[i].running && is_idle(cpus[i].running))
        {
            cpu_kick(i);
            return;
        }
    }
}

static void place_vruntime(process_t *p)
{
    uint64_t lowest;

    if (is_weighted(p) && lowest_vruntime(p, &lowest) && p->vruntime < lowest)
    {
        p->vruntime = lowest;
    }

    if (p->state == PROCESS_READY && scheduler_running)
    {
        kick_for(p);
    }
}

static process_t *pick_next(void)
{
    int start = is_idle(current) ? 0 : (int)(current - processes);

    for (int i = 1; i <= PROCESS_MAX; i++)
    {
        process_t *p = &processes[(start + i) % PROCESS_MAX];

        if (p->state == PROCESS_READY &&
            p->process_class == PROCESS_CLASS_INTERACTIVE && runs_here(p))
        {
            return p;
        }
    }

    process_t *best = 0;

    for (int i = 0; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if (p->state != PROCESS_READY || !is_weighted(p) || !runs_here(p))
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

    return cpu_self()->idle;
}

static void account_time(cpu_t *cpu, process_t *leaving)
{
    uint64_t now = timer_read();

    if (cpu->switch_time != 0)
    {
        if (is_idle(leaving))
        {
            cpu->idle_time += now - cpu->switch_time;
        }
        else
        {
            cpu->busy_time += now - cpu->switch_time;
        }
    }

    cpu->switch_time = now;
}

static void cpu_times(cpu_t *cpu, uint64_t *busy, uint64_t *idle)
{
    uint64_t now = timer_read();
    uint64_t running = cpu->switch_time != 0 ? now - cpu->switch_time : 0;

    *busy = cpu->busy_time;
    *idle = cpu->idle_time;

    if (cpu->running && !is_idle(cpu->running))
    {
        *busy += running;
    }
    else
    {
        *idle += running;
    }
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

    count_switches++;
    account_time(cpu_self(), previous);
    cpu_self()->switches++;
    next->cpu = cpu_id();
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
    idle->cpu = 0;

    cpu_self()->idle = idle;
    cpu_self()->switch_time = timer_read();
    current = idle;
    guardian_set_current(idle->pid, idle->name);
}

void process_init_cpu(int id)
{
    process_t *idle = &idle_processes[id];

    idle->pid = 0;
    copy_name(idle->name, "idle");
    idle->process_class = PROCESS_CLASS_IDLE;
    idle->state = PROCESS_RUNNING;
    idle->stack = 0;
    idle->slice_left = class_info[PROCESS_CLASS_IDLE].slice;
    idle->satp = vm_kernel_satp();
    idle->cpu = id;
    cpus[id].idle = idle;
    cpus[id].switch_time = timer_read();
    cpus[id].running = idle;
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

        if (is_running_anywhere(candidate) || candidate->space_pending)
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
    p->syscalls = 0;
    p->seen_cpu = 0;
    p->seen_syscalls = 0;
    p->spawned = 0;
    p->seen_spawned = 0;
    p->disk_bytes = 0;
    p->seen_disk_bytes = 0;
    p->cwd[0] = '/';
    p->cwd[1] = 0;
    p->capture_owner = 0;
    p->capture_pid = 0;
    p->capture_quiet = 0;
    p->capture_length = 0;
    p->cpu = -1;
    p->pin_mask = 0;
    p->kill_pending = 0;
    p->leader = 0;
    p->space_pending = 0;
    p->thread_stack = 0;
    p->thread_argument = 0;
    p->linux_state = 0;
    p->user_sp = 0;

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

int process_create_pinned(const char *name,
                          process_class_t process_class,
                          process_entry_t entry,
                          void *arg,
                          uint32_t cpu_mask)
{
    uint64_t enabled = interrupts_disable();
    process_t *p = create_locked(name, process_class, entry, arg);
    int pid = -1;

    if (p != 0)
    {
        p->pin_mask = cpu_mask;
        place_vruntime(p);
        pid = p->pid;
    }

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
    if (p->linux_state != 0)
    {
        linux_destroy(p->linux_state);
        p->linux_state = 0;
    }

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
    char path[PATH_MAX] = "/bin/";
    uint32_t inode;
    knocfs_stat_t stat;
    int length = 5;

    if (program->path)
    {
        length = 0;

        while (program->path[length] && length < (int)sizeof(path) - 1)
        {
            path[length] = program->path[length];
            length++;
        }
    }
    else
    {
        for (int i = 0; program->name[i] && length < (int)sizeof(path) - 1; i++)
        {
            path[length++] = program->name[i];
        }
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

static void *load_file(const char *path, uint64_t *size)
{
    uint32_t inode;
    knocfs_stat_t stat;

    if (!knocfs_mounted() || knocfs_lookup(path, &inode) != 0 || knocfs_stat(inode, &stat) != 0 ||
        stat.type != KNOCFS_TYPE_FILE || stat.size == 0 || stat.size > PROGRAM_FILE_MAX)
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

static uint8_t *page_in(process_t *owner, uintptr_t address, uint64_t rwx, int merge)
{
    pte_t entry;

    if (vm_page_get(owner->user_root, address, &entry) == 0)
    {
        if (!merge)
        {
            return 0;
        }

        vm_page_protect(owner->user_root, address, (entry & (PTE_R | PTE_W | PTE_X)) | rwx);
        return (uint8_t *)PPN_TO_PA(entry >> 10);
    }

    if (owner->mem_used + PAGE_SIZE > owner->mem_limit)
    {
        return 0;
    }

    uint8_t *page = page_alloc();

    if (page == 0)
    {
        return 0;
    }

    memset(page, 0, PAGE_SIZE);

    if (vm_page_set(owner->user_root, address, (uintptr_t)page, rwx | PTE_OWNED) != 0)
    {
        page_free(page);
        return 0;
    }

    owner->mem_used += PAGE_SIZE;
    return page;
}

static uint8_t *linux_page(void *context, uintptr_t address, uint64_t rwx)
{
    return page_in((process_t *)context, address, rwx, 1);
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

    int linux = elf_is_linux(image, size);
    elf_linux_info_t info;
    int loaded = 0;

    p->user_sp = 0;

    if (p->user_root != 0 && linux)
    {
        p->mem_limit = LINUX_QUOTA;
        loaded = elf_load_linux(image, size, LINUX_PIE_BASE, linux_page, p, &info) == 0;
        p->user_entry = info.entry;
        info.interp_base = 0;

        if (loaded && info.interp[0])
        {
            uint64_t interp_size = 0;
            void *interp = load_file(info.interp, &interp_size);
            elf_linux_info_t interp_info;

            loaded = interp != 0 &&
                     elf_load_linux(interp, interp_size, LINUX_INTERP_BASE, linux_page, p, &interp_info) == 0;

            if (interp != 0)
            {
                page_free(interp);
            }

            if (loaded)
            {
                p->user_entry = interp_info.entry;
                info.interp_base = interp_info.base;
            }
            else
            {
                uart_puts("[LINUX] ");
                uart_puts(program->name);
                uart_puts(" needs the Linux loader ");
                uart_puts(info.interp);
                uart_puts(": not found (scripts/get-linux-base.sh installs it)\n");
            }
        }
    }
    else if (p->user_root != 0)
    {
        loaded = elf_load(p->user_root, image, size, user_block, p, &p->user_entry) == 0;
    }

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

    if (linux)
    {
        uint8_t *linux_stack = user_block(p, LINUX_STACK_SIZE);
        char exe[PATH_MAX] = "/bin/";

        if (program->path)
        {
            memcpy(exe, program->path, PATH_MAX - 1);
        }
        else
        {
            copy_name(exe + 5, program->name);
        }

        if (linux_stack == 0 ||
            vm_user_map(p->user_root, USER_STACK_TOP - LINUX_STACK_SIZE, (uintptr_t)linux_stack, LINUX_STACK_SIZE,
                        PTE_R | PTE_W) != 0 ||
            (p->linux_state = linux_create(exe, program->name, p->args, p->cwd, &info, linux_stack, USER_STACK_TOP,
                                           LINUX_STACK_SIZE, &p->user_sp)) == 0)
        {
            user_space_free(p);
            return -1;
        }

        p->satp = vm_make_satp(p->user_root);
        return 0;
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

    uintptr_t sp = current->user_sp ? current->user_sp : current->leader ? current->thread_stack : USER_STACK_TOP;

    bkl_leave_to_user();
    user_enter(current->user_entry, sp, kernel_sp, current->thread_argument);
}

int process_spawn(const program_t *program)
{
    return process_spawn_args(program, "");
}

const char *process_args(void)
{
    return current->args;
}

static int spawn(const program_t *program, const char *args, int capture);

int process_spawn_args(const program_t *program, const char *args)
{
    return spawn(program, args, 0);
}

int process_spawn_capture(const program_t *program, const char *args, int quiet)
{
    return spawn(program, args, quiet ? 2 : 1);
}

static int spawn(const program_t *program, const char *args, int capture)
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

    if (current != 0)
    {
        memcpy(p->cwd, current->cwd, PATH_MAX);
    }

    if (capture && current != 0)
    {
        p->capture_owner = current->pid;
        p->capture_quiet = capture == 2;
        current->capture_pid = p->pid;
        current->capture_length = 0;
    }

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

    if ((program->flags & PROGRAM_TERMINAL) && args[0] == 0)
    {
        tty_set_owner(pid);
    }

    interrupts_restore(enabled);

    count_spawns++;

    if (current != 0)
    {
        current->spawned++;
    }

    memgraph_record(GRAPH_KIND_ACTOR, current != 0 && current->user ? current->name : "kernel",
                    GRAPH_REL_STARTED, GRAPH_KIND_PROGRAM, program->name, "kernel", 100);
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

    open_file_t *file = &owner_of(current)->files[fd - FD_FIRST_FILE];

    return file->used ? file : 0;
}

int process_file_open(uint32_t inode, uint32_t flags)
{
    for (int i = 0; i < PROCESS_FILES_MAX; i++)
    {
        open_file_t *file = &owner_of(current)->files[i];

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
    count_syscalls++;
    current->syscalls++;
    current->trace[current->trace_count % PROCESS_TRACE_MAX] =
        number < 255 ? (uint8_t)number : 255;
    current->trace_count++;
}

static process_t *find_live(int pid);

int process_resolve_path(const char *path, char *out)
{
    char joined[PATH_MAX * 2];
    unsigned long length = 0;

    if (path[0] != '/')
    {
        for (int i = 0; current->cwd[i] && length < sizeof(joined) - 2; i++)
        {
            joined[length++] = current->cwd[i];
        }

        joined[length++] = '/';
    }

    for (int i = 0; path[i] && length < sizeof(joined) - 1; i++)
    {
        joined[length++] = path[i];
    }

    joined[length] = 0;

    unsigned long out_length = 0;
    unsigned long i = 0;

    while (joined[i])
    {
        while (joined[i] == '/')
        {
            i++;
        }

        unsigned long start = i;

        while (joined[i] && joined[i] != '/')
        {
            i++;
        }

        unsigned long part = i - start;

        if (part == 0 || (part == 1 && joined[start] == '.'))
        {
            continue;
        }

        if (part == 2 && joined[start] == '.' && joined[start + 1] == '.')
        {
            while (out_length > 0 && out[out_length - 1] != '/')
            {
                out_length--;
            }

            if (out_length > 0)
            {
                out_length--;
            }

            continue;
        }

        if (out_length + part + 2 > PATH_MAX)
        {
            return E_INVAL;
        }

        out[out_length++] = '/';
        memcpy(out + out_length, joined + start, part);
        out_length += part;
    }

    if (out_length == 0)
    {
        out[out_length++] = '/';
    }

    out[out_length] = 0;
    return 0;
}

int process_chdir(const char *path)
{
    memcpy(current->cwd, path, PATH_MAX);
    return 0;
}

const char *process_cwd(void)
{
    return current->cwd;
}

int process_capture(const char *data, uint64_t length)
{
    if (current == 0 || current->capture_owner == 0)
    {
        return 0;
    }

    uint64_t enabled = interrupts_disable();
    process_t *owner = find_live(current->capture_owner);

    if (owner != 0 && owner->capture_pid == current->pid)
    {
        for (uint64_t i = 0; i < length && owner->capture_length < PROCESS_CAPTURE_MAX; i++)
        {
            owner->capture[owner->capture_length++] = data[i];
        }
    }

    int quiet = owner != 0 && owner->capture_pid == current->pid && current->capture_quiet;

    interrupts_restore(enabled);
    return quiet;
}

uint64_t process_captured(char *out, uint64_t length)
{
    uint64_t count = current->capture_length < length ? current->capture_length : length;

    memcpy(out, current->capture, count);
    return count;
}

void process_note_disk(uint64_t bytes)
{
    current->disk_bytes += bytes;
}

void process_note_denied(void)
{
    count_denied++;
    current->denied++;
}

int64_t process_mem_alloc(uint64_t bytes)
{
    if (bytes == 0 || bytes > PROCESS_QUOTA_AI_AGENT)
    {
        return E_INVAL;
    }

    process_t *owner = owner_of(current);
    uint64_t size = block_size(bytes);
    uintptr_t alignment = size >= VM_MEGAPAGE_SIZE ? VM_MEGAPAGE_SIZE : PAGE_SIZE;
    uintptr_t address = (owner->heap_next + alignment - 1) & ~(alignment - 1);

    if (address + size > USER_HEAP_END)
    {
        return E_NOMEM;
    }

    void *memory = user_block(owner, size);

    if (memory == 0)
    {
        return E_NOMEM;
    }

    if (vm_user_map(owner->user_root, address, (uintptr_t)memory, size, PTE_R | PTE_W) != 0)
    {
        return E_NOMEM;
    }

    asm volatile("sfence.vma zero, zero");

    owner->heap_next = address + size;
    return (int64_t)address;
}

void scheduler_start(void)
{
    log_info("Scheduler started (AI-aware: INTERACTIVE first, then AI_AGENT 60 / NORMAL 30 / BACKGROUND 10)");
    scheduler_running = 1;
}

void scheduler_tick(void)
{
    if (!scheduler_running)
    {
        return;
    }

    cpu_t *cpu = cpu_self();
    int interactive_woke = 0;

    if (cpu->id == 0)
    {
        uint64_t now = timer_ticks();

        if (now % 10 == 0)
        {
            net_tick();
        }

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
    }

    current->cpu_ticks++;

    if (is_idle(current))
    {
        cpu->idle_ticks++;
    }
    else
    {
        cpu->busy_ticks++;
    }

    if (is_weighted(current))
    {
        current->vruntime += VRUNTIME_SCALE / class_info[current->process_class].weight;
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

    if (is_idle(current))
    {
        need_switch = 1;
    }

    if (need_switch)
    {
        schedule();
    }
}

void scheduler_kick(void)
{
    cpu_self()->kicked = 0;

    if (scheduler_running && is_idle(current))
    {
        schedule();
    }
}

int scheduler_idle_tick(void)
{
    cpu_t *cpu = cpu_self();

    cpu->kicked = 0;

    if (cpu->id == 0 || cpu->bkl || !scheduler_running || !is_idle(current) || cpu->resched)
    {
        return 0;
    }

    for (int i = 0; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if (p->state == PROCESS_READY && runs_here(p))
        {
            return 0;
        }
    }

    current->cpu_ticks++;
    cpu->idle_ticks++;
    return 1;
}

int process_can_block(void)
{
    return scheduler_running && current != 0 && !is_idle(current);
}

int process_block(void *channel, uint64_t timeout)
{
    uint64_t enabled = interrupts_disable();

    current->wait_channel = channel;
    current->wake_tick = timeout != 0 ? timer_ticks() + timeout : 0;
    current->woken = 0;
    current->state = PROCESS_BLOCKED;
    current->block_seq = ++block_count;

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

        /* Run it right away if this core is idle or it has a higher class than the running process */
        if (runs_here(p) && (is_idle(current) || p->process_class < current->process_class))
        {
            cpu_self()->resched = 1;
        }
    }

    interrupts_restore(enabled);
}

/* Called at the end of a device interrupt: switch now if a wake-up asked for it */
void scheduler_preempt(void)
{
    if (scheduler_running && cpu_self()->resched)
    {
        cpu_self()->resched = 0;
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

static void track_lock(process_t *p, sleeplock_t *lock)
{
    for (int i = 0; i < PROCESS_LOCKS_MAX; i++)
    {
        if (p->locks[i] == 0)
        {
            p->locks[i] = lock;
            return;
        }
    }
}

void sleeplock_acquire(sleeplock_t *lock)
{
    uint64_t enabled = interrupts_disable();

    while (1)
    {
        if (!lock->locked)
        {
            lock->locked = 1;
            lock->owner = current != 0 ? current->pid : 0;

            if (current != 0)
            {
                track_lock(current, lock);
            }

            break;
        }

        if (current != 0 && lock->owner == current->pid)
        {
            break;
        }

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

    interrupts_restore(enabled);
}

void sleeplock_release(sleeplock_t *lock)
{
    uint64_t enabled = interrupts_disable();
    process_t *next = 0;

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

    for (int i = 0; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if (p->state == PROCESS_BLOCKED && p->wait_channel == lock &&
            (next == 0 || p->block_seq < next->block_seq))
        {
            next = p;
        }
    }

    if (next == 0)
    {
        lock->locked = 0;
        lock->owner = 0;
    }
    else
    {
        lock->owner = next->pid;
        track_lock(next, lock);
        next->woken = 1;
        next->state = PROCESS_READY;
        place_vruntime(next);

        if (runs_here(next) &&
            (is_idle(current) ||
             (next->process_class == PROCESS_CLASS_INTERACTIVE &&
              current->process_class != PROCESS_CLASS_INTERACTIVE)))
        {
            cpu_self()->resched = 1;
        }
    }

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

static void release_locks(process_t *p);

static int live_threads(process_t *owner, process_t *except)
{
    int count = 0;

    for (int i = 0; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if (p != except && p->leader == owner && p->state != PROCESS_UNUSED && p->state != PROCESS_EXITED)
        {
            count++;
        }
    }

    return count;
}

static void release_space(process_t *owner)
{
    if (live_threads(owner, 0) == 0)
    {
        user_space_free(owner);
        owner->space_pending = 0;
    }
    else
    {
        owner->space_pending = 1;
    }
}

static void end_thread(process_t *p, int code)
{
    process_t *owner = p->leader;

    net_release(p->pid);
    release_locks(p);
    p->exit_code = code;
    p->state = PROCESS_EXITED;
    process_wake(p);

    if (owner->space_pending && live_threads(owner, 0) == 0)
    {
        user_space_free(owner);
        owner->space_pending = 0;
    }
}

static void stop_threads(process_t *owner)
{
    for (int i = 0; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if (p->leader != owner || p->state == PROCESS_UNUSED || p->state == PROCESS_EXITED || p == current)
        {
            continue;
        }

        if (p->state == PROCESS_RUNNING)
        {
            p->kill_pending = 1;
        }
        else
        {
            end_thread(p, E_KILLED);
        }
    }
}

int process_thread_spawn(uintptr_t entry, uintptr_t argument, uintptr_t stack)
{
    if (current == 0 || !current->user)
    {
        return E_INVAL;
    }

    uint64_t enabled = interrupts_disable();
    process_t *owner = owner_of(current);

    if (live_threads(owner, 0) >= PROCESS_THREADS_MAX)
    {
        interrupts_restore(enabled);
        return E_NOMEM;
    }

    process_t *p = create_locked(owner->name, owner->process_class, user_process_start, 0);

    if (p == 0)
    {
        interrupts_restore(enabled);
        return E_NOMEM;
    }

    p->user = 1;
    p->leader = owner;
    p->program = owner->program;
    p->user_root = owner->user_root;
    p->satp = owner->satp;
    p->capabilities = owner->capabilities;
    p->mem_limit = 0;
    p->user_entry = entry;
    p->thread_stack = stack & ~0xFUL;
    p->thread_argument = argument;
    p->pin_mask = owner->pin_mask;
    p->capture_owner = owner->capture_owner;
    p->capture_quiet = owner->capture_quiet;
    memcpy(p->cwd, owner->cwd, PATH_MAX);
    memcpy(p->args, owner->args, ARGS_MAX);

    int pid = p->pid;

    count_spawns++;
    interrupts_restore(enabled);
    return pid;
}

void process_exit_code(int code)
{
    interrupts_disable();

    current->exit_code = code;

    if (current->user && current->leader)
    {
        vm_switch(vm_kernel_satp());
        end_thread(current, code);
        schedule();

        while (1)
        {
        }
    }

    if (current->user)
    {
        /* Leave the program's page table before freeing it */
        vm_switch(vm_kernel_satp());
        net_release(current->pid);
        stop_threads(current);
        release_space(current);
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
    if (p->state == PROCESS_RUNNING)
    {
        p->kill_pending = 1;
        return 0;
    }

    if (p->leader)
    {
        end_thread(p, E_KILLED);
        return 0;
    }

    net_release(p->pid);

    if (p->user)
    {
        stop_threads(p);
        release_space(p);
    }

    release_locks(p);

    p->exit_code = E_KILLED;
    p->state = PROCESS_EXITED;
    process_wake(p);
    return 0;
}

int process_is_linux(void)
{
    return current != 0 && current->linux_state != 0;
}

void *process_linux_state(void)
{
    return current != 0 ? current->linux_state : 0;
}

uint8_t *process_page_new(uintptr_t address, uint64_t rwx)
{
    return page_in(owner_of(current), address, rwx, 0);
}

int process_page_reserve(uintptr_t address)
{
    return vm_page_set(owner_of(current)->user_root, address, 0, 0);
}

int process_page_free(uintptr_t address)
{
    process_t *owner = owner_of(current);
    pte_t old;

    if (vm_page_clear(owner->user_root, address, &old) != 0)
    {
        return -1;
    }

    if (old & PTE_OWNED)
    {
        page_free((void *)PPN_TO_PA(old >> 10));
        owner->mem_used -= owner->mem_used >= PAGE_SIZE ? PAGE_SIZE : owner->mem_used;
    }

    return 0;
}

int process_page_protect(uintptr_t address, uint64_t rwx)
{
    process_t *owner = owner_of(current);
    pte_t entry;

    if (vm_page_get(owner->user_root, address, &entry) != 0)
    {
        return -1;
    }

    if (!(entry & PTE_OWNED) && PPN_TO_PA(entry >> 10) == 0)
    {
        if (rwx == 0)
        {
            return 0;
        }

        vm_page_clear(owner->user_root, address, &entry);
        return page_in(owner, address, rwx, 0) ? 0 : -1;
    }

    return vm_page_protect(owner->user_root, address, rwx);
}

int process_page_exists(uintptr_t address)
{
    pte_t entry;

    return vm_page_get(owner_of(current)->user_root, address, &entry) == 0;
}

void process_exit_if_killed(void)
{
    if (current == 0 || !current->kill_pending)
    {
        return;
    }

    interrupts_disable();
    current->kill_pending = 0;
    release_locks(current);
    process_exit_code(E_KILLED);
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

int process_lower_class_user(int pid, uint32_t process_class)
{
    if (process_class > PROCESS_CLASS_BACKGROUND)
    {
        return E_INVAL;
    }

    uint64_t enabled = interrupts_disable();
    process_t *p = find_live(pid);
    int result = E_NOTFOUND;

    if (p != 0)
    {
        if (!p->user || process_class < (uint32_t)p->process_class)
        {
            result = E_PERM;
        }
        else
        {
            p->process_class = (process_class_t)process_class;
            result = 0;
        }
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

static uint64_t since(uint64_t *seen, uint64_t now)
{
    uint64_t delta = now - *seen;

    *seen = now;
    return delta;
}

static void copy_telemetry_name(char *to, const char *from)
{
    int i = 0;

    while (from[i] && i < TELEMETRY_NAME_MAX - 1)
    {
        to[i] = from[i];
        i++;
    }

    to[i] = 0;
}

void process_telemetry(telemetry_sample_t *sample)
{
    static uint64_t seen_switches;
    static uint64_t seen_syscall_count;
    static uint64_t seen_denied;
    static uint64_t seen_spawns;
    static uint64_t seen_crashes;
    uint64_t enabled = interrupts_disable();
    uint64_t elapsed = since(&seen_ticks, timer_ticks());
    uint64_t best_cpu = 0;
    uint64_t best_sys = 0;
    uint64_t best_mem = 0;
    uint64_t best_spawn = 0;
    uint64_t best_disk = 0;
    uint64_t idle = 0;

    if (elapsed == 0)
    {
        elapsed = 1;
    }

    sample->switches = (uint32_t)since(&seen_switches, count_switches);
    sample->syscalls = (uint32_t)since(&seen_syscall_count, count_syscalls);
    sample->denied = (uint32_t)since(&seen_denied, count_denied);
    sample->spawns = (uint32_t)since(&seen_spawns, count_spawns);
    sample->crashes = (uint32_t)since(&seen_crashes, count_crashes);
    sample->processes = 0;
    sample->user_memory_kib = 0;
    sample->top_cpu_pid = sample->top_mem_pid = sample->top_sys_pid = sample->top_spawn_pid = -1;
    sample->top_cpu_name[0] = sample->top_mem_name[0] = sample->top_sys_name[0] = 0;
    sample->top_spawn_name[0] = 0;
    sample->top_disk_pid = -1;
    sample->top_disk_name[0] = 0;

    for (int i = 0; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];
        uint64_t cpu = since(&p->seen_cpu, p->cpu_ticks);
        uint64_t sys = since(&p->seen_syscalls, p->syscalls);
        uint64_t spawned = since(&p->seen_spawned, p->spawned);
        uint64_t disk = since(&p->seen_disk_bytes, p->disk_bytes);

        if (p->state == PROCESS_UNUSED || p->state == PROCESS_EXITED)
        {
            continue;
        }

        if (i == 0)
        {
            idle = cpu;
            continue;
        }

        sample->processes++;
        sample->user_memory_kib += p->mem_used / 1024;

        if (cpu > best_cpu)
        {
            best_cpu = cpu;
            sample->top_cpu_pid = p->pid;
            copy_telemetry_name(sample->top_cpu_name, p->name);
        }

        if (sys > best_sys)
        {
            best_sys = sys;
            sample->top_sys_pid = p->pid;
            copy_telemetry_name(sample->top_sys_name, p->name);
        }

        if (disk > best_disk)
        {
            best_disk = disk;
            sample->top_disk_pid = p->pid;
            copy_telemetry_name(sample->top_disk_name, p->name);
        }

        if (spawned > best_spawn)
        {
            best_spawn = spawned;
            sample->top_spawn_pid = p->pid;
            copy_telemetry_name(sample->top_spawn_name, p->name);
        }

        if (p->mem_used > best_mem)
        {
            best_mem = p->mem_used;
            sample->top_mem_pid = p->pid;
            copy_telemetry_name(sample->top_mem_name, p->name);
        }
    }

    uint32_t busiest = 0;

    for (int i = 0; i < CPU_MAX; i++)
    {
        static uint64_t seen_busy[CPU_MAX];
        static uint64_t seen_idle[CPU_MAX];
        uint64_t busy_now;
        uint64_t idle_now;

        cpu_times(&cpus[i], &busy_now, &idle_now);

        uint64_t busy = since(&seen_busy[i], busy_now);
        uint64_t idle_part = since(&seen_idle[i], idle_now);

        if (cpus[i].online && ((CPU_MASK_GENERAL >> i) & 1) && busy + idle_part > 0)
        {
            uint32_t percent = (uint32_t)(busy * 100 / (busy + idle_part));

            if (percent > busiest)
            {
                busiest = percent;
            }
        }
    }

    interrupts_restore(enabled);

    (void)idle;
    sample->cpu_busy = busiest;
    sample->top_cpu = (uint32_t)(best_cpu * 100 / elapsed);
    sample->top_sys = (uint32_t)(best_sys * 100 / elapsed);
    sample->top_mem_kib = (uint32_t)(best_mem / 1024);
    sample->top_spawn = (uint32_t)best_spawn;
    sample->top_disk_kib = (uint32_t)(best_disk / 1024);
}

int process_info(uint32_t index, process_info_t *info)
{
    int foreground = tty_foreground();
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
        info->flags = (p->pid == foreground ? PROCESS_FLAG_FOREGROUND : 0) | (p->linux_state ? PROCESS_FLAG_LINUX : 0);
        info->cpu_ticks = p->cpu_ticks;
        info->memory = p->mem_used;
        info->denied = p->denied;
        info->reserved = (uint32_t)(p->cpu + 1);
        memset(info->name, 0, sizeof(info->name));
        copy_name(info->name, p->name);

        interrupts_restore(enabled);
        return 0;
    }

    interrupts_restore(enabled);
    return -1;
}

int process_cpu_info(uint32_t index, cpu_info_t *info)
{
    if (index >= CPU_MAX)
    {
        return -1;
    }

    uint64_t enabled = interrupts_disable();
    cpu_t *cpu = &cpus[index];

    memset(info, 0, sizeof(*info));
    info->id = index;
    info->role = index == AISPACE_HART ? CPU_ROLE_AI_SPACE
                 : ((CPU_MASK_AI >> index) & 1) ? CPU_ROLE_AI
                                                : CPU_ROLE_GENERAL;
    info->online = index == AISPACE_HART ? (uint32_t)guardian_ai_online() : (uint32_t)cpu->online;
    uint64_t busy;
    uint64_t idle;

    cpu_times(cpu, &busy, &idle);
    info->busy_ticks = busy / TIMER_INTERVAL;
    info->idle_ticks = idle / TIMER_INTERVAL;
    info->running_pid = -1;

    if (cpu->online && cpu->running && !is_idle(cpu->running))
    {
        info->running_pid = cpu->running->pid;
        copy_name(info->running, cpu->running->name);
    }

    interrupts_restore(enabled);
    return 0;
}

int process_can_contain_fault(void)
{
    return scheduler_running && current != 0 && !is_idle(current);
}

void process_crash(uint64_t scause, uint64_t sepc, uint64_t stval, uint64_t sp)
{
    interrupts_disable();

    if (current->leader)
    {
        process_t *owner = current->leader;

        if (owner->state != PROCESS_EXITED && owner->state != PROCESS_UNUSED && owner->state != PROCESS_CRASHED)
        {
            kill_locked(owner);
        }

        vm_switch(vm_kernel_satp());
        end_thread(current, E_CRASHED);
        schedule();

        while (1)
        {
        }
    }

    count_crashes++;
    net_release(current->pid);
    stop_threads(current);
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
    current->fault_sp = sp;
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
        fault->sp = p->fault_sp;
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

    while (live_threads(p, 0) > 0 && process_can_block())
    {
        process_sleep(1);
    }

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
            release_space(p);
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
    return current != 0 ? current->pid : 0;
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

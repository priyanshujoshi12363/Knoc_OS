#include "process.h"
#include "heap.h"
#include "timer.h"
#include "trap.h"
#include "uart.h"
#include "logging.h"
#include "guardian.h"

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
};

static process_t processes[PROCESS_MAX];
static process_t *current = 0;
static int next_pid = 1;
static int scheduler_running = 0;
static const char *boot_driver = 0;

extern void context_switch(process_context_t *old_context,
                           process_context_t *new_context);

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

    current = idle;
    guardian_set_current(idle->pid, idle->name);
}

int process_create(const char *name,
                   process_class_t process_class,
                   process_entry_t entry,
                   void *arg)
{
    uint64_t enabled = interrupts_disable();
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
        interrupts_restore(enabled);
        return -1;
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
        interrupts_restore(enabled);
        return -1;
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
    reset_context(p);
    p->state = PROCESS_READY;

    place_vruntime(p);

    int pid = p->pid;

    interrupts_restore(enabled);
    return pid;
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

        if (p->state == PROCESS_SLEEPING && now >= p->wake_tick)
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

void process_exit(void)
{
    interrupts_disable();

    current->state = PROCESS_EXITED;
    schedule();

    while (1)
    {
    }
}

int process_kill(int pid)
{
    uint64_t enabled = interrupts_disable();

    for (int i = 1; i < PROCESS_MAX; i++)
    {
        process_t *p = &processes[i];

        if (p->state == PROCESS_UNUSED || p->state == PROCESS_EXITED)
        {
            continue;
        }

        if (p->pid == pid && p != current)
        {
            p->state = PROCESS_EXITED;
            interrupts_restore(enabled);
            return 0;
        }
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

    interrupts_restore(enabled);
    return new_pid;
}

void process_discard(int pid)
{
    uint64_t enabled = interrupts_disable();
    process_t *p = find_crashed(pid);

    if (p != 0)
    {
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

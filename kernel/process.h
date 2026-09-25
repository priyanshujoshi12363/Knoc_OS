#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

#define PROCESS_MAX 16
#define PROCESS_NAME_MAX 16
#define PROCESS_CAPTURE_MAX 4096
#define PROCESS_STACK_SIZE (16 * 1024)
#define PROCESS_TRACE_MAX 8
#define PROCESS_BLOCKS_MAX 32
#define PROCESS_LOCKS_MAX 4
#define PROCESS_FILES_MAX 8

/* Memory quota for user programs, by class: AI agents get room for models */
#define PROCESS_QUOTA_AI_AGENT (1024UL * 1024 * 1024)
#define PROCESS_QUOTA_DEFAULT (16UL * 1024 * 1024)

#define SCHED_WEIGHT_AI_AGENT 60
#define SCHED_WEIGHT_NORMAL 30
#define SCHED_WEIGHT_BACKGROUND 10

#define SCHED_SLICE_INTERACTIVE 1
#define SCHED_SLICE_AI_AGENT 3
#define SCHED_SLICE_NORMAL 2
#define SCHED_SLICE_BACKGROUND 1

typedef enum process_class
{
    PROCESS_CLASS_INTERACTIVE,
    PROCESS_CLASS_AI_AGENT,
    PROCESS_CLASS_NORMAL,
    PROCESS_CLASS_BACKGROUND,
    PROCESS_CLASS_IDLE,
} process_class_t;

typedef enum process_state
{
    PROCESS_UNUSED,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_SLEEPING,
    PROCESS_EXITED,
    PROCESS_CRASHED,
    PROCESS_LOADING,
    PROCESS_BLOCKED,
} process_state_t;

/* A lock a process can hold while it sleeps (for example during disk I/O).
   Waiters block on it instead of spinning. */
typedef struct sleeplock
{
    volatile int locked;
    int owner;
} sleeplock_t;

#define SLEEPLOCK_INIT {0, 0}

typedef struct open_file
{
    int used;
    uint32_t inode;
    uint32_t flags;
    uint64_t offset;
} open_file_t;

typedef struct process_context
{
    uint64_t ra;
    uint64_t sp;
    uint64_t s[12];
} process_context_t;

typedef void (*process_entry_t)(void *arg);

typedef struct process_fault
{
    int pid;
    char name[PROCESS_NAME_MAX];
    char driver[PROCESS_NAME_MAX];
    uint64_t scause;
    uint64_t sepc;
    uint64_t stval;
    uint32_t restarts;
    int user;
    uint32_t denied;
    uint32_t trace_count;
    uint8_t trace[PROCESS_TRACE_MAX];
} process_fault_t;

struct program;

void process_init(void);
int process_create(const char *name,
                   process_class_t process_class,
                   process_entry_t entry,
                   void *arg);

void scheduler_start(void);
void scheduler_tick(void);

void process_yield(void);

/* Wait queues: block until process_wake(channel), or until the timeout
   (in ticks, 0 = none). Check the condition and block with interrupts
   off, so a wake-up can't slip in between. Returns 1 if woken by an event. */
int process_block(void *channel, uint64_t timeout);
void process_wake(void *channel);
int process_can_block(void);
void scheduler_preempt(void);
uint64_t process_block_count(void);
void process_wait_crash(uint64_t timeout);

void sleeplock_acquire(sleeplock_t *lock);
void sleeplock_release(sleeplock_t *lock);
void process_sleep(uint64_t ticks);
void process_exit(void);
void process_exit_code(int code) __attribute__((noreturn));
int process_spawn(const struct program *program);
int process_spawn_args(const struct program *program, const char *args);
int process_spawn_capture(const struct program *program, const char *args);
const char *process_args(void);
int process_wait(int pid, int *exit_code);
int process_kill(int pid);
int process_kill_user(int pid);
int process_lower_class_user(int pid, uint32_t process_class);
int process_alive(int pid);

int process_can_contain_fault(void);
void process_crash(uint64_t scause, uint64_t sepc, uint64_t stval)
    __attribute__((noreturn));
int process_next_crash(process_fault_t *fault);
int process_restart(int pid);
void process_discard(int pid);

const char *process_driver_enter(const char *name);
void process_driver_leave(const char *previous);
const char *process_current_driver(void);

int process_current_pid(void);
const char *process_current_name(void);

int process_is_user(void);
uintptr_t process_user_root(void);
uint32_t process_capabilities(void);
void process_record_syscall(uint64_t number);
void process_note_denied(void);
void process_note_disk(uint64_t bytes);
void process_capture(const char *data, uint64_t length);
uint64_t process_captured(char *out, uint64_t length);
int64_t process_mem_alloc(uint64_t bytes);

open_file_t *process_file(int fd);
int process_file_open(uint32_t inode, uint32_t flags);
uint64_t process_disk_loads(void);

struct process_info;
int process_info(uint32_t index, struct process_info *info);

struct telemetry_sample;
void process_telemetry(struct telemetry_sample *sample);
uint64_t process_cpu_ticks(int pid);
void process_list(void);

#endif

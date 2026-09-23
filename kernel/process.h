#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

#define PROCESS_MAX 16
#define PROCESS_NAME_MAX 16
#define PROCESS_STACK_SIZE (16 * 1024)
#define PROCESS_TRACE_MAX 8
#define PROCESS_BLOCKS_MAX 32

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
} process_state_t;

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
void process_sleep(uint64_t ticks);
void process_exit(void);
void process_exit_code(int code) __attribute__((noreturn));
int process_spawn(const struct program *program);
int process_wait(int pid, int *exit_code);
int process_kill(int pid);

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
int64_t process_mem_alloc(uint64_t bytes);
uint64_t process_cpu_ticks(int pid);
void process_list(void);

#endif

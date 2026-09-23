#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

#define PROCESS_MAX 16
#define PROCESS_NAME_MAX 16
#define PROCESS_STACK_SIZE (16 * 1024)

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
} process_state_t;

typedef struct process_context
{
    uint64_t ra;
    uint64_t sp;
    uint64_t s[12];
} process_context_t;

typedef void (*process_entry_t)(void *arg);

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
int process_kill(int pid);

int process_current_pid(void);
uint64_t process_cpu_ticks(int pid);
void process_list(void);

#endif

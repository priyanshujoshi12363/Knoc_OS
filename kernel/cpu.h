#ifndef CPU_H
#define CPU_H

#define CPU_MAX 8
#define CPU_STACK_SIZE 16384
#define CPU_MASK_GENERAL 0x0FU
#define CPU_MASK_AI 0xE0U
#define CLINT_BASE 0x02000000UL
#define CLINT_SIZE 0x10000UL

#ifndef __ASSEMBLER__

#include <stdint.h>

struct process;

typedef struct cpu
{
    int id;
    volatile int online;
    int bkl;
    int resched;
    struct process *running;
    struct process *idle;
    uint64_t busy_ticks;
    uint64_t idle_ticks;
    uint64_t switches;
    uint64_t last_cmp;
    volatile int kicked;
    uint64_t pass_after;
    uint64_t busy_time;
    uint64_t idle_time;
    uint64_t switch_time;
} cpu_t;

extern cpu_t cpus[CPU_MAX];

static inline cpu_t *cpu_self(void)
{
    uint64_t id;

    asm volatile("mv %0, tp" : "=r"(id));
    return &cpus[id];
}

static inline int cpu_id(void)
{
    uint64_t id;

    asm volatile("mv %0, tp" : "=r"(id));
    return (int)id;
}

void cpu_init_boot(void);
void cpu_set_present(int count);
void cpu_start_secondaries(void);
extern volatile uint32_t cpu_online_bits;
uint32_t cpu_online_mask(void);
int cpu_online_count(void);
void cpu_idle_loop(void) __attribute__((noreturn));
void cpu_kick(int id);
uint64_t cpu_ticks_passed(void);

void bkl_acquire(void);
void bkl_release(void);
void bkl_enter(void);
void bkl_leave_to_user(void);
void bkl_pass(void);
int bkl_waiting(void);

#endif

#endif

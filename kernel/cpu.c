#include "cpu.h"
#include "trap.h"
#include "vm.h"
#include "timer.h"
#include "mailbox.h"
#include "aispace.h"
#include "process.h"
#include "logging.h"
#include "uart.h"
#include "spinlock.h"

#define SECONDARY_START_TIMEOUT (TIMER_FREQ_HZ * 2)

cpu_t cpus[CPU_MAX];
volatile uint32_t cpu_online_bits = 1;

static int present_count = 1;
static volatile uint32_t bkl_next;
static volatile uint32_t bkl_serving;

void bkl_acquire(void)
{
    uint32_t ticket = __atomic_fetch_add(&bkl_next, 1, __ATOMIC_RELAXED);

    while (__atomic_load_n(&bkl_serving, __ATOMIC_ACQUIRE) != ticket)
    {
    }

    cpu_self()->bkl = 1;
}

void bkl_release(void)
{
    cpu_self()->bkl = 0;
    __atomic_store_n(&bkl_serving, bkl_serving + 1, __ATOMIC_RELEASE);
}

int bkl_waiting(void)
{
    return __atomic_load_n(&bkl_next, __ATOMIC_RELAXED) - __atomic_load_n(&bkl_serving, __ATOMIC_RELAXED) > 1;
}

void bkl_enter(void)
{
    if (!cpu_self()->bkl)
    {
        bkl_acquire();
    }
}

void bkl_leave_to_user(void)
{
    if (cpu_self()->bkl)
    {
        bkl_release();
    }
}

void bkl_pass(void)
{
    cpu_t *cpu = cpu_self();

    if (cpu->bkl && bkl_waiting() && timer_read() >= cpu->pass_after)
    {
        bkl_release();
        bkl_acquire();
        cpu->pass_after = timer_read() + TIMER_INTERVAL;
    }
}

void cpu_init_boot(void)
{
    for (int i = 0; i < CPU_MAX; i++)
    {
        cpus[i].id = i;
    }

    cpus[0].online = 1;
    cpus[0].bkl = 0;
    bkl_acquire();
}

uint32_t cpu_online_mask(void)
{
    uint32_t mask = 0;

    for (int i = 0; i < CPU_MAX; i++)
    {
        if (cpus[i].online)
        {
            mask |= 1U << i;
        }
    }

    return mask;
}

int cpu_online_count(void)
{
    int count = 0;

    for (int i = 0; i < CPU_MAX; i++)
    {
        count += cpus[i].online != 0;
    }

    return count;
}

void cpu_kick(int id)
{
    if (id != cpu_id() && id < CPU_MAX && cpus[id].online && !cpus[id].kicked)
    {
        cpus[id].kicked = 1;
        ((volatile uint32_t *)CLINT_BASE)[id] = 1;
    }
}

uint64_t cpu_ticks_passed(void)
{
    cpu_t *cpu = cpu_self();
    uint64_t cmp = *(volatile uint64_t *)(CLINT_BASE + 0x4000 + 8 * (uint64_t)cpu->id);
    uint64_t passed = cpu->last_cmp ? (cmp - cpu->last_cmp) / TIMER_INTERVAL : 1;

    cpu->last_cmp = cmp;
    return passed;
}

void cpu_idle_loop(void)
{
    while (1)
    {
        asm volatile("csrc sstatus, %0" :: "r"((uint64_t)SSTATUS_SIE));

        if (cpu_self()->bkl)
        {
            bkl_release();
        }

        asm volatile("wfi");
        asm volatile("csrs sstatus, %0" :: "r"((uint64_t)SSTATUS_SIE));
    }
}

void secondary_main(uint64_t hart)
{
    cpu_t *cpu = &cpus[hart];

    vm_switch(vm_kernel_satp());
    bkl_acquire();
    process_init_cpu(cpu->id);
    cpu->online = 1;
    __atomic_fetch_or(&cpu_online_bits, 1U << hart, __ATOMIC_SEQ_CST);
    guardian_mailbox.harts_online |= 1U << hart;
    asm volatile("csrs sie, %0" :: "r"((uint64_t)SIE_SSIE));
    cpu_idle_loop();
}

void cpu_set_present(int count)
{
    present_count = count < CPU_MAX ? count : CPU_MAX;
}

void cpu_start_secondaries(void)
{
    uint32_t expected = 0;

    guardian_mailbox.harts_online = 1;

    for (int i = 1; i < CPU_MAX; i++)
    {
        if (i != AISPACE_HART && i < present_count)
        {
            expected |= 1U << i;
        }
    }

    uint64_t interrupts = irq_save();

    __sync_synchronize();
    guardian_mailbox.smp_go = 1;

    bkl_release();

    uint64_t start = timer_read();

    while ((cpu_online_mask() & expected) != expected && timer_read() - start < SECONDARY_START_TIMEOUT)
    {
    }

    bkl_acquire();
    irq_restore(interrupts);

    int general = 0;
    int ai = 0;

    for (int i = 0; i < CPU_MAX; i++)
    {
        if (cpus[i].online)
        {
            general += (CPU_MASK_GENERAL >> i) & 1;
            ai += (CPU_MASK_AI >> i) & 1;
        }
    }

    interrupts = irq_save();
    uart_puts("[INFO] CPU cores online: ");
    uart_put_uint((uint64_t)cpu_online_count());
    uart_puts(" kernel cores (");
    uart_put_uint((uint64_t)general);
    uart_puts(" general: 0-3, ");
    uart_put_uint((uint64_t)ai);
    uart_puts(" AI: 5-7) + the AI space on core ");
    uart_put_uint(AISPACE_HART);
    uart_puts("\n");
    irq_restore(interrupts);
}

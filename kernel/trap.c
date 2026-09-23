#include "trap.h"
#include "logging.h"
#include "timer.h"
#include "plic.h"
#include "device.h"
#include "process.h"
#include "guardian.h"
#include "uart.h"
#include "syscall.h"

#define SCAUSE_INTERRUPT (1UL << 63)
#define SCAUSE_CODE_MASK (~SCAUSE_INTERRUPT)

#define EXCEPTION_BREAKPOINT 3
#define EXCEPTION_LOAD_ACCESS_FAULT 5
#define EXCEPTION_LOAD_PAGE_FAULT 13
#define EXCEPTION_USER_ECALL 8
#define INTERRUPT_SUPERVISOR_SOFTWARE 1
#define INTERRUPT_SUPERVISOR_EXTERNAL 9
#define INSTRUCTION_SIZE 4

static volatile uint64_t breakpoint_count = 0;
static volatile int probe_active = 0;
static volatile int probe_faulted = 0;

static const char *exception_names[] = {
    [0] = "Instruction address misaligned",
    [1] = "Instruction access fault",
    [2] = "Illegal instruction",
    [3] = "Breakpoint",
    [4] = "Load address misaligned",
    [5] = "Load access fault",
    [6] = "Store address misaligned",
    [7] = "Store access fault",
    [8] = "Environment call from U-mode",
    [9] = "Environment call from S-mode",
    [12] = "Instruction page fault",
    [13] = "Load page fault",
    [15] = "Store page fault",
};

static const char *interrupt_names[] = {
    [1] = "Supervisor software interrupt",
    [5] = "Supervisor timer interrupt",
    [9] = "Supervisor external interrupt",
};

#define EXCEPTION_NAME_COUNT \
    (sizeof(exception_names) / sizeof(exception_names[0]))

#define INTERRUPT_NAME_COUNT \
    (sizeof(interrupt_names) / sizeof(interrupt_names[0]))

static uint64_t read_scause(void)
{
    uint64_t value;
    asm volatile("csrr %0, scause" : "=r"(value));
    return value;
}

static uint64_t read_sepc(void)
{
    uint64_t value;
    asm volatile("csrr %0, sepc" : "=r"(value));
    return value;
}

static uint64_t read_stval(void)
{
    uint64_t value;
    asm volatile("csrr %0, stval" : "=r"(value));
    return value;
}

static uint64_t read_sstatus(void)
{
    uint64_t value;
    asm volatile("csrr %0, sstatus" : "=r"(value));
    return value;
}

static void write_sstatus(uint64_t value)
{
    asm volatile("csrw sstatus, %0" :: "r"(value));
}

static void write_sepc(uint64_t value)
{
    asm volatile("csrw sepc, %0" :: "r"(value));
}

static void clear_sip(uint64_t bits)
{
    asm volatile("csrc sip, %0" :: "r"(bits));
}

static void handle_external_interrupt(void)
{
    uint32_t irq = plic_claim();

    if (irq == 0)
    {
        return;
    }

    if (device_handle_irq(irq) != 0)
    {
        log_warn("Unexpected external interrupt");
    }

    plic_complete(irq);
}

static const char *trap_name(uint64_t scause)
{
    uint64_t code = scause & SCAUSE_CODE_MASK;
    const char *name = 0;

    if (scause & SCAUSE_INTERRUPT)
    {
        if (code < INTERRUPT_NAME_COUNT)
        {
            name = interrupt_names[code];
        }
    }
    else
    {
        if (code < EXCEPTION_NAME_COUNT)
        {
            name = exception_names[code];
        }
    }

    if (name == 0)
    {
        return "Unknown trap";
    }

    return name;
}

void supervisor_trap_handler(trap_frame_t *frame)
{
    uint64_t scause = read_scause();
    uint64_t sepc = read_sepc();
    uint64_t stval = read_stval();
    uint64_t sstatus = read_sstatus();

    if ((scause & SCAUSE_INTERRUPT) &&
        (scause & SCAUSE_CODE_MASK) == INTERRUPT_SUPERVISOR_SOFTWARE)
    {
        clear_sip(SIP_SSIP);
        timer_tick();
        guardian_heartbeat();
        scheduler_tick();

        write_sepc(sepc);
        write_sstatus(sstatus);
        return;
    }

    if ((scause & SCAUSE_INTERRUPT) &&
        (scause & SCAUSE_CODE_MASK) == INTERRUPT_SUPERVISOR_EXTERNAL)
    {
        handle_external_interrupt();
        scheduler_preempt();

        write_sepc(sepc);
        write_sstatus(sstatus);
        return;
    }

    if (probe_active &&
        !(scause & SCAUSE_INTERRUPT) &&
        ((scause & SCAUSE_CODE_MASK) == EXCEPTION_LOAD_ACCESS_FAULT ||
         (scause & SCAUSE_CODE_MASK) == EXCEPTION_LOAD_PAGE_FAULT))
    {
        probe_faulted = 1;

        write_sepc(sepc + INSTRUCTION_SIZE);
        write_sstatus(sstatus);
        return;
    }

    if (!(scause & SCAUSE_INTERRUPT) &&
        (scause & SCAUSE_CODE_MASK) == EXCEPTION_BREAKPOINT)
    {
        breakpoint_count++;

        log_trap("Breakpoint");
        log_trap_hex("sepc   = ", sepc);

        write_sepc(sepc + INSTRUCTION_SIZE);
        write_sstatus(sstatus);
        return;
    }

    /* From a user program (sstatus.SPP = 0) */
    if (!(scause & SCAUSE_INTERRUPT) && !(sstatus & SSTATUS_SPP))
    {
        if ((scause & SCAUSE_CODE_MASK) == EXCEPTION_USER_ECALL)
        {
            frame->a0 = (uint64_t)syscall_handle(frame);

            write_sepc(sepc + INSTRUCTION_SIZE);
            write_sstatus(sstatus);
            return;
        }

        /* A user program can't have touched kernel memory: always contain it */
        uart_puts("[OOPS] ");
        uart_puts(trap_name(scause));
        uart_puts(" in user program ");
        uart_puts(process_current_name());
        uart_puts(" (pid ");
        uart_put_uint((uint64_t)process_current_pid());
        uart_puts("): stopping only this program\n");
        log_trap_hex("sepc   = ", sepc);
        log_trap_hex("stval  = ", stval);

        process_crash(scause, sepc, stval);
    }

    if (!(scause & SCAUSE_INTERRUPT) &&
        (sstatus & SSTATUS_SPIE) &&
        process_can_contain_fault())
    {
        const char *driver = process_current_driver();

        uart_puts("[OOPS] ");
        uart_puts(trap_name(scause));
        uart_puts(" in process ");
        uart_puts(process_current_name());
        uart_puts(" (pid ");
        uart_put_uint((uint64_t)process_current_pid());
        uart_puts(")");

        if (driver != 0)
        {
            uart_puts(" inside driver ");
            uart_puts(driver);
        }

        uart_puts(": stopping only this process\n");
        log_trap_hex("sepc   = ", sepc);
        log_trap_hex("stval  = ", stval);

        process_crash(scause, sepc, stval);
    }

    guardian_record_trap(scause, sepc, stval, frame->ra, frame->sp);

    log_trap(trap_name(scause));
    log_trap_hex("scause = ", scause);
    log_trap_hex("sepc   = ", sepc);
    log_trap_hex("stval  = ", stval);
    log_trap_hex("ra     = ", frame->ra);
    log_trap_hex("sp     = ", frame->sp);

    panic("Unhandled supervisor trap");
}

uint64_t trap_breakpoint_count(void)
{
    return breakpoint_count;
}

int trap_probe_read(uintptr_t address, uint64_t *value)
{
    uint64_t result = 0;

    probe_faulted = 0;
    probe_active = 1;

    asm volatile("ld %0, 0(%1)" : "=r"(result) : "r"(address) : "memory");

    probe_active = 0;

    if (probe_faulted)
    {
        return -1;
    }

    *value = result;
    return 0;
}

void trap_enable_interrupts(void)
{
    asm volatile("csrs sie, %0" :: "r"(SIE_SSIE | SIE_SEIE));
    asm volatile("csrs sstatus, %0" :: "r"(SSTATUS_SIE));
}

#ifndef MAILBOX_H
#define MAILBOX_H

/* boot.S uses these offsets: boot_request and boot_ack must stay first */
#define MAILBOX_BOOT_REQUEST 0
#define MAILBOX_BOOT_ACK 8

#ifndef __ASSEMBLER__

#include <stdint.h>

#define MAILBOX_MAGIC 0x584F424C49414D4BULL
#define MAILBOX_NAME_MAX 16
#define MAILBOX_MESSAGE_MAX 64
#define MAILBOX_DISABLED_MAX 4
#define MAILBOX_TRACE_MAX 8

#define KERNEL_STATE_BOOTING 0
#define KERNEL_STATE_RUNNING 1
#define KERNEL_STATE_PANICKED 2

#define AISPACE_STATE_OFFLINE 0
#define AISPACE_STATE_ONLINE 1
#define AISPACE_STATE_HANDLING 2

#define CRASH_TYPE_NONE 0
#define CRASH_TYPE_PANIC 1
#define CRASH_TYPE_TRAP 2
#define CRASH_TYPE_FREEZE 3

#define VERDICT_RESTART_PROCESS 0x1
#define VERDICT_DISABLE_DRIVER 0x2

#define CONSOLE_FREE 0
#define CONSOLE_KERNEL 1
#define CONSOLE_AISPACE 2

typedef struct guardian_mailbox
{
    /* Boot handshake: the kernel waits for the AI space to copy it first */
    volatile uint64_t boot_request;
    volatile uint64_t boot_ack;

    volatile uint64_t aispace_magic;
    volatile uint32_t aispace_state;
    volatile uint32_t kernel_state;
    volatile uint32_t watch_enabled;
    volatile uint32_t safe_mode;
    volatile uint64_t heartbeat;
    volatile uint64_t uptime_ticks;
    volatile uint64_t last_kernel_pc;
    volatile int32_t current_pid;
    volatile char current_name[MAILBOX_NAME_MAX];

    /* Kernel crash details */
    volatile uint32_t crash_type;
    volatile uint64_t scause;
    volatile uint64_t sepc;
    volatile uint64_t stval;
    volatile uint64_t ra;
    volatile uint64_t sp;
    volatile char message[MAILBOX_MESSAGE_MAX];
    volatile char driver[MAILBOX_NAME_MAX];

    /* Warm restart: written by the AI space before it releases core 0 */
    volatile uint64_t ai_start_time;
    volatile uint32_t kernel_restarts;
    volatile uint32_t restart_safe_mode;
    volatile uint32_t streak_reset;
    volatile char disabled_drivers[MAILBOX_DISABLED_MAX][MAILBOX_NAME_MAX];

    /* Core 0 parking, used to stop the kernel before a warm restart */
    volatile uint32_t core0_parked;
    volatile uint32_t core0_release;

    /* Process fault: kernel -> AI space */
    volatile uint64_t fault_seq;
    volatile int32_t fault_pid;
    volatile uint32_t fault_restarts;
    volatile char fault_name[MAILBOX_NAME_MAX];
    volatile char fault_driver[MAILBOX_NAME_MAX];
    volatile uint64_t fault_scause;
    volatile uint64_t fault_sepc;
    volatile uint64_t fault_stval;

    /* Verdict: AI space -> kernel */
    volatile uint64_t verdict_seq;
    volatile uint32_t verdict_action;

    /* Which core prints a line on the UART right now (CONSOLE_*) */
    volatile uint32_t console_owner;

    /* End of RAM, from the device tree (written by the kernel) */
    volatile uint64_t ram_end;

    /* Process fault, user programs: forbidden calls and the last system calls */
    volatile uint32_t fault_user;
    volatile uint32_t fault_denied;
    volatile uint32_t fault_trace_count;
    volatile uint8_t fault_trace[MAILBOX_TRACE_MAX];
} guardian_mailbox_t;

extern guardian_mailbox_t guardian_mailbox;

#endif

#endif

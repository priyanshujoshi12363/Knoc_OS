#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdint.h>

#define MAILBOX_MAGIC 0x584F424C49414D4BULL
#define MAILBOX_NAME_MAX 16
#define MAILBOX_MESSAGE_MAX 64

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

typedef struct guardian_mailbox
{
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
    volatile uint32_t crash_type;
    volatile uint64_t scause;
    volatile uint64_t sepc;
    volatile uint64_t stval;
    volatile uint64_t ra;
    volatile uint64_t sp;
    volatile char message[MAILBOX_MESSAGE_MAX];
} guardian_mailbox_t;

extern guardian_mailbox_t guardian_mailbox;

#endif

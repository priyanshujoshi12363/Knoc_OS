#ifndef BLACKBOX_H
#define BLACKBOX_H

#include <stdint.h>

#define BLACKBOX_MAGIC 0x584F424B43414C42ULL
#define BLACKBOX_SECTORS 8
#define BLACKBOX_RECORDS 4
#define BLACKBOX_SECTOR_SIZE 512
#define BLACKBOX_SAFE_MODE_THRESHOLD 3
#define BLACKBOX_NAME_MAX 16
#define BLACKBOX_MESSAGE_MAX 64
#define BLACKBOX_DIAGNOSIS_MAX 160

#define BLACKBOX_ACTION_REBOOT 1
#define BLACKBOX_ACTION_SAFE_MODE 2
#define BLACKBOX_ACTION_HALT 3

typedef struct blackbox_header
{
    uint64_t magic;
    uint64_t total_crashes;
    uint64_t reported_crashes;
    uint64_t consecutive_crashes;
} blackbox_header_t;

typedef struct blackbox_record
{
    uint64_t magic;
    uint64_t sequence;
    uint32_t crash_type;
    uint32_t action;
    uint64_t scause;
    uint64_t sepc;
    uint64_t stval;
    uint64_t ra;
    uint64_t sp;
    uint64_t kernel_pc;
    uint64_t uptime_ticks;
    int32_t pid;
    char process_name[BLACKBOX_NAME_MAX];
    char message[BLACKBOX_MESSAGE_MAX];
    char diagnosis[BLACKBOX_DIAGNOSIS_MAX];
} blackbox_record_t;

static inline uint64_t blackbox_header_sector(uint64_t capacity)
{
    return capacity - BLACKBOX_SECTORS;
}

static inline uint64_t blackbox_record_sector(uint64_t capacity, uint64_t sequence)
{
    return capacity - BLACKBOX_SECTORS + 1 + ((sequence - 1) % BLACKBOX_RECORDS);
}

#endif

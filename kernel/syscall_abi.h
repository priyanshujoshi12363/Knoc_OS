#ifndef SYSCALL_ABI_H
#define SYSCALL_ABI_H

/* Shared by the kernel, the AI space and user programs.
   A program puts the call number in a7 and arguments in a0-a5, runs
   ecall, and gets the result in a0 (negative = error). */

#define SYS_EXIT 0
#define SYS_WRITE 1
#define SYS_READ 2
#define SYS_GETPID 3
#define SYS_YIELD 4
#define SYS_SLEEP 5
#define SYS_UPTIME 6
#define SYS_SPAWN 7
#define SYS_MEM_ALLOC 8
#define SYS_OPEN 9
#define SYS_CLOSE 10
#define SYS_SEEK 11
#define SYS_STAT 12
#define SYS_READDIR 13
#define SYS_MKDIR 14
#define SYS_REMOVE 15
#define SYS_WAIT 16
#define SYS_PS 17
#define SYS_KILL 18
#define SYS_SYSINFO 19
#define SYS_DEVINFO 20
#define SYS_CRASHINFO 21
#define SYS_RANDOM 22
#define SYS_INJECT 23
#define SYS_COUNT 24

/* File descriptors: 0 = keyboard, 1 and 2 = screen, 3+ = open files */
#define FD_STDIN 0
#define FD_STDOUT 1
#define FD_STDERR 2
#define FD_FIRST_FILE 3

#define O_READ 0x1
#define O_WRITE 0x2
#define O_CREATE 0x4
#define O_TRUNC 0x8

#define FILE_TYPE_FILE 1
#define FILE_TYPE_DIR 2
#define PATH_MAX 128
#define FILE_NAME_MAX 60

#define E_BADCALL -1
#define E_FAULT -2
#define E_PERM -3
#define E_NOMEM -4
#define E_NOTFOUND -5
#define E_INVAL -6
#define E_EXISTS -7
#define E_NOSPACE -8
#define E_BADF -9
#define E_ISDIR -10
#define E_NOTDIR -11
#define E_NOTEMPTY -12
#define E_IO -13
#define E_CRASHED -14
#define E_KILLED -15

/* Capabilities: what a program is allowed to ask the kernel for */
#define CAP_CONSOLE 0x1
#define CAP_SPAWN 0x2
#define CAP_MEMORY 0x4
#define CAP_FILES_READ 0x8
#define CAP_FILES_WRITE 0x10
#define CAP_SYSTEM 0x20
#define CAP_DEBUG 0x40

/* User address space (Sv39 root slots 64-127, never used by the kernel) */
#define USER_BASE 0x1000000000UL
#define USER_CODE_END 0x1040000000UL
#define USER_HEAP_BASE 0x1100000000UL
#define USER_HEAP_END 0x1F00000000UL
#define USER_STACK_TOP 0x1F80000000UL
#define USER_STACK_SIZE (64UL * 1024)
#define USER_END 0x2000000000UL

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct file_stat
{
    uint32_t type;
    uint32_t extents;
    uint64_t size;
} file_stat_t;

typedef struct dir_entry
{
    char name[FILE_NAME_MAX];
    uint32_t type;
    uint64_t size;
} dir_entry_t;

/* System information for the shell (CAP_SYSTEM) */

#define INFO_NAME_MAX 16
#define INFO_DISABLED_MAX 4

typedef struct process_info
{
    int32_t pid;
    uint32_t process_class;
    uint32_t state;
    uint32_t user;
    uint32_t restarts;
    uint32_t reserved;
    uint64_t cpu_ticks;
    uint64_t memory;
    char name[INFO_NAME_MAX];
} process_info_t;

typedef struct system_info
{
    uint64_t ram_bytes;
    uint64_t ram_free_bytes;
    uint64_t disk_bytes;
    uint64_t disk_free_bytes;
    uint64_t uptime_ticks;
    uint64_t ai_uptime_ms;
    uint32_t disk_files;
    uint32_t cpu_count;
    uint32_t ai_online;
    uint32_t kernel_restarts;
    uint32_t crashes_total;
    uint32_t crashes_in_a_row;
    uint32_t safe_mode;
    uint32_t reserved;
    char disabled_drivers[INFO_DISABLED_MAX][INFO_NAME_MAX];
} system_info_t;

typedef struct device_info
{
    char name[INFO_NAME_MAX];
    uint32_t irq;
    uint32_t ready;
    uint32_t disabled;
    uint32_t reserved;
    uint64_t blocks;
} device_info_t;

typedef struct crash_info
{
    uint64_t sequence;
    uint64_t uptime_ticks;
    uint32_t crash_type;
    uint32_t action;
    int32_t pid;
    uint32_t reserved;
    char process[INFO_NAME_MAX];
    char driver[INFO_NAME_MAX];
    char message[64];
    char diagnosis[160];
} crash_info_t;

#endif

#endif

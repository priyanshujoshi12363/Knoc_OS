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
#define SYS_GETARGS 22
#define SYS_RENAME 23
#define SYS_GRAPH 24
#define SYS_TELEMETRY 25
#define SYS_SETCLASS 26
#define SYS_SPAWN_CAPTURE 27
#define SYS_CAPTURED 28
#define SYS_CHDIR 29
#define SYS_GETCWD 30
#define SYS_NET_INFO 31
#define SYS_NET_RESOLVE 32
#define SYS_NET_PING 33
#define SYS_TCP_CONNECT 34
#define SYS_TCP_SEND 35
#define SYS_TCP_RECV 36
#define SYS_TCP_CLOSE 37
#define SYS_TIME 38
#define SYS_GETRANDOM 39
#define SYS_CPUINFO 40
#define SYS_THREAD 41
#define SYS_COUNT 42

#define RANDOM_MAX 256

/* File descriptors: 0 = keyboard, 1 and 2 = screen, 3+ = open files */
#define FD_STDIN 0
#define FD_STDOUT 1
#define FD_STDERR 2
#define FD_FIRST_FILE 3

#define O_READ 0x1
#define O_WRITE 0x2
#define O_CREATE 0x4
#define O_TRUNC 0x8
#define SEEK_POSITION ((unsigned long)-1)
#define SEEK_SIZE ((unsigned long)-2)

#define FILE_TYPE_FILE 1
#define FILE_TYPE_DIR 2
#define PATH_MAX 128
#define ARGS_MAX 256
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
#define E_TIMEOUT -16
#define E_REFUSED -17
#define E_NETDOWN -18
#define E_NODEV -19

/* Capabilities: what a program is allowed to ask the kernel for */
#define CAP_CONSOLE 0x1
#define CAP_SPAWN 0x2
#define CAP_MEMORY 0x4
#define CAP_FILES_READ 0x8
#define CAP_FILES_WRITE 0x10
#define CAP_SYSTEM 0x20
#define CAP_KNOWLEDGE 0x40
#define CAP_NET 0x80

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
#define CLASS_BACKGROUND 3
#define PROCESS_FLAG_FOREGROUND 0x1
#define INFO_DISABLED_MAX 4

typedef struct process_info
{
    int32_t pid;
    uint32_t process_class;
    uint32_t state;
    uint32_t user;
    uint32_t restarts;
    uint32_t flags;
    uint64_t cpu_ticks;
    uint64_t memory;
    uint32_t denied;
    uint32_t reserved;
    char name[INFO_NAME_MAX];
} process_info_t;

#define CPU_ROLE_GENERAL 0
#define CPU_ROLE_AI 1
#define CPU_ROLE_AI_SPACE 2

typedef struct cpu_info
{
    uint32_t id;
    uint32_t online;
    uint32_t role;
    int32_t running_pid;
    uint64_t busy_ticks;
    uint64_t idle_ticks;
    char running[INFO_NAME_MAX];
} cpu_info_t;

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

#define GRAPH_NAME_MAX 96

#define GRAPH_KIND_FILE 1
#define GRAPH_KIND_FOLDER 2
#define GRAPH_KIND_PROGRAM 3
#define GRAPH_KIND_DRIVER 4
#define GRAPH_KIND_CRASH 5
#define GRAPH_KIND_TYPE 6
#define GRAPH_KIND_SOURCE 7
#define GRAPH_KIND_ACTOR 8
#define GRAPH_KIND_DIAGNOSIS 9
#define GRAPH_KIND_ACTION 10
#define GRAPH_KIND_CAPABILITY 11
#define GRAPH_KIND_COUNT 12

#define GRAPH_KIND_NAMES \
    {"any", "file", "folder", "program", "driver", "crash", "type", "source", \
     "actor", "diagnosis", "action", "capability"}

#define GRAPH_REL_CLASSIFIED_AS 1
#define GRAPH_REL_CAME_FROM 2
#define GRAPH_REL_MOVED_TO 3
#define GRAPH_REL_RESTORED_TO 4
#define GRAPH_REL_STARTED 5
#define GRAPH_REL_CRASHED 6
#define GRAPH_REL_CRASHED_IN 7
#define GRAPH_REL_DIAGNOSED_AS 8
#define GRAPH_REL_ACTION 9
#define GRAPH_REL_DISABLED 10
#define GRAPH_REL_DENIED 11
#define GRAPH_REL_RESTARTED 12
#define GRAPH_REL_STOPPED 13
#define GRAPH_REL_IN_PROCESS 14
#define GRAPH_REL_ANOMALY 15
#define GRAPH_REL_LOWERED 16
#define GRAPH_REL_WORKED_IN 17
#define GRAPH_REL_COUNT 18

#define GRAPH_REL_NAMES \
    {"?", "classified_as", "came_from", "moved_to", "restored_to", "started", "crashed", \
     "crashed_in", "diagnosed_as", "action", "disabled", "denied", "restarted", "stopped", "in_process", \
     "anomaly", "lowered", "worked_in"}

#define GRAPH_OP_RECORD 1
#define GRAPH_OP_STATS 2
#define GRAPH_OP_RECENT 3
#define GRAPH_OP_EDGES 4
#define GRAPH_OP_FIND 5
#define GRAPH_OP_FORGET 6

typedef struct graph_request
{
    uint32_t op;
    uint32_t index;
    uint32_t kind_a;
    uint32_t relation;
    uint32_t kind_b;
    uint32_t confidence;
    char a[GRAPH_NAME_MAX];
    char b[GRAPH_NAME_MAX];
} graph_request_t;

typedef struct graph_node_info
{
    uint32_t kind;
    uint32_t hits;
    uint32_t edges;
    uint32_t first_boot;
    uint32_t last_boot;
    uint32_t reserved;
    char name[GRAPH_NAME_MAX];
} graph_node_info_t;

typedef struct graph_edge_info
{
    uint32_t seq;
    uint32_t boot;
    uint64_t uptime;
    uint32_t relation;
    uint32_t confidence;
    uint32_t from_kind;
    uint32_t to_kind;
    char from[GRAPH_NAME_MAX];
    char to[GRAPH_NAME_MAX];
    char actor[GRAPH_NAME_MAX];
} graph_edge_info_t;

typedef struct graph_stats
{
    uint32_t nodes;
    uint32_t edges;
    uint32_t node_capacity;
    uint32_t edge_capacity;
    uint32_t boot;
    uint32_t reserved;
    uint32_t by_kind[GRAPH_KIND_COUNT];
} graph_stats_t;

typedef struct net_info
{
    uint32_t up;
    uint32_t address;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    uint32_t connections;
    uint8_t mac[8];
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t bytes_in;
    uint64_t bytes_out;
} net_info_t;

#define TELEMETRY_NAME_MAX 16

typedef struct telemetry_sample
{
    uint32_t seq;
    uint32_t uptime;
    uint32_t cpu_busy;
    uint32_t switches;
    uint32_t syscalls;
    uint32_t denied;
    uint32_t processes;
    uint32_t spawns;
    uint32_t crashes;
    uint32_t disk_reads;
    uint32_t disk_writes;
    uint32_t disk_wait;
    uint64_t ram_free_kib;
    uint64_t ram_total_kib;
    uint64_t user_memory_kib;
    uint64_t disk_free_kib;
    uint64_t disk_total_kib;
    int32_t top_cpu_pid;
    uint32_t top_cpu;
    int32_t top_mem_pid;
    uint32_t top_mem_kib;
    int32_t top_sys_pid;
    uint32_t top_sys;
    int32_t top_spawn_pid;
    uint32_t top_spawn;
    char top_cpu_name[TELEMETRY_NAME_MAX];
    char top_mem_name[TELEMETRY_NAME_MAX];
    char top_sys_name[TELEMETRY_NAME_MAX];
    char top_spawn_name[TELEMETRY_NAME_MAX];
    int32_t top_disk_pid;
    uint32_t top_disk_kib;
    char top_disk_name[TELEMETRY_NAME_MAX];
} telemetry_sample_t;

#endif

#endif

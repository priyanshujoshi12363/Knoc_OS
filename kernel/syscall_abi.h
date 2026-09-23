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
#define SYS_COUNT 9

#define E_BADCALL -1
#define E_FAULT -2
#define E_PERM -3
#define E_NOMEM -4
#define E_NOTFOUND -5
#define E_INVAL -6

/* Capabilities: what a program is allowed to ask the kernel for */
#define CAP_CONSOLE 0x1
#define CAP_SPAWN 0x2
#define CAP_MEMORY 0x4

/* User address space (Sv39 root slots 64-127, never used by the kernel) */
#define USER_BASE 0x1000000000UL
#define USER_CODE_END 0x1040000000UL
#define USER_HEAP_BASE 0x1100000000UL
#define USER_HEAP_END 0x1F00000000UL
#define USER_STACK_TOP 0x1F80000000UL
#define USER_STACK_SIZE (64UL * 1024)
#define USER_END 0x2000000000UL

#endif

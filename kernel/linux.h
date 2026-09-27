#ifndef LINUX_H
#define LINUX_H

#include <stdint.h>
#include "trap.h"
#include "elf.h"

#define LINUX_STACK_SIZE (1024UL * 1024)
#define LINUX_PIE_BASE 0x40000000UL
#define LINUX_INTERP_BASE 0x70000000UL
#define LINUX_MMAP_BASE 0x1800000000UL
#define LINUX_QUOTA (256UL * 1024 * 1024)

void *linux_create(const char *exe, const char *name, const char *args, const char *cwd,
                   const elf_linux_info_t *info, uint8_t *stack, uintptr_t stack_top, uint64_t stack_size,
                   uintptr_t *sp);
void linux_destroy(void *state);
int64_t linux_syscall(trap_frame_t *frame);

#endif

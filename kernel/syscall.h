#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "trap.h"

int64_t syscall_handle(trap_frame_t *frame);
const char *syscall_name(uint64_t number);

#endif

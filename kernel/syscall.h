#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "trap.h"

int64_t syscall_handle(trap_frame_t *frame);
const char *syscall_name(uint64_t number);

int user_copy_in(void *destination, uintptr_t source, uint64_t length);
int user_copy_out(uintptr_t destination, const void *source, uint64_t length);
int user_copy_string(char *destination, uintptr_t source, uint64_t max);
int syscall_allowed(uint64_t number, uint32_t capability);
int64_t kfile_open(const char *path, uint64_t flags);
int64_t kfile_path_change(uint64_t number, const char *path);
int64_t kfile_rename(const char *from, const char *to);
int64_t kfile_read(uint64_t fd, uintptr_t buffer, uint64_t length);
int64_t kfile_write(uint64_t fd, uintptr_t buffer, uint64_t length);
int64_t kfile_close(uint64_t fd);

#endif

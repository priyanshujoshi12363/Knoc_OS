#ifndef ULIB_H
#define ULIB_H

#include <stdint.h>
#include "../kernel/syscall_abi.h"

long syscall(long number, long a0, long a1, long a2);

void exit(int code) __attribute__((noreturn));
long write(int fd, const void *buffer, unsigned long length);
long read(int fd, void *buffer, unsigned long length);
int open(const char *path, int flags);
int close(int fd);
long seek(int fd, unsigned long offset);
int stat(const char *path, file_stat_t *stat);
int readdir(const char *path, unsigned long index, dir_entry_t *entry);
int mkdir(const char *path);
int remove(const char *path);
int getpid(void);
void yield(void);
void sleep(unsigned long ticks);
unsigned long uptime(void);
int spawn(const char *name);
void *mem_alloc(unsigned long bytes);

void print(const char *text);
void print_uint(unsigned long value);
void print_hex(unsigned long value);
int is_error(long result);

void *memset(void *destination, int value, unsigned long length);
void *memcpy(void *destination, const void *source, unsigned long length);

#endif

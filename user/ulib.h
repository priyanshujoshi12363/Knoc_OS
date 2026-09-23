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
long wait(int pid);
int ps(unsigned long index, process_info_t *info);
int kill(int pid);
int sysinfo(system_info_t *info);
int devinfo(unsigned long index, device_info_t *info);
int crashinfo(unsigned long index, crash_info_t *info);
int getpid(void);
void yield(void);
void sleep(unsigned long ticks);
unsigned long uptime(void);
int spawn(const char *name);
int spawn_args(const char *name, const char *args);
long getargs(char *buffer, unsigned long length);
int rename(const char *from, const char *to);
void *mem_alloc(unsigned long bytes);

void print(const char *text);
void print_uint(unsigned long value);
void print_hex(unsigned long value);
int is_error(long result);

unsigned long strlen(const char *text);
int memcmp_bytes(const void *a, const void *b, unsigned long length);
int strcmp(const char *a, const char *b);
char *strcpy(char *destination, const char *source);
long parse_number(const char *text);
void print_padded(const char *text, int width);
void print_padded_uint(unsigned long value, int width);

void *memset(void *destination, int value, unsigned long length);
void *memcpy(void *destination, const void *source, unsigned long length);

#endif

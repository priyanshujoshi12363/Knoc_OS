#ifndef LOGGING_H
#define LOGGING_H

#include <stdint.h>

void log_info(const char *message);
void log_warn(const char *message);
void log_trap(const char *message);
void log_trap_hex(const char *label, uint64_t value);
void panic(const char *message);

#endif
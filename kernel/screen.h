#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>
#include "syscall_abi.h"

int screen_info(screen_info_t *info);
int64_t screen_map(void);
int screen_flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void screen_release(int pid);
int64_t screen_input(input_event_t *events, uint32_t max, uint64_t timeout);
int screen_cursor(const uint32_t *pixels, uint32_t hot_x, uint32_t hot_y);

#endif

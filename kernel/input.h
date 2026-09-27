#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include "syscall_abi.h"

void input_key(uint16_t code, int32_t value);
void input_absolute(int axis, int32_t value);
void input_button(uint16_t code, int32_t value);
void input_wheel(int32_t value);
void input_sync(void);
int64_t input_read(input_event_t *events, uint32_t max, uint64_t timeout);
void input_claim(int pid);
void input_release(int pid);
int input_owner(void);

#endif

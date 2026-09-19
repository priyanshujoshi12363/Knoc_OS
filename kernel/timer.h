#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

uint64_t timer_read(void);
void timer_set_next(uint64_t value);
void timer_interrupt(void);
uint64_t timer_ticks(void);

#endif
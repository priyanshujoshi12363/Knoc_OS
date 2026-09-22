#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#define TIMER_FREQ_HZ 10000000ULL
#define TIMER_TICK_HZ 100ULL
#define TIMER_INTERVAL (TIMER_FREQ_HZ / TIMER_TICK_HZ)

uint64_t timer_read(void);
void timer_set_next(uint64_t value);
void timer_interrupt(void);
uint64_t timer_ticks(void);

#endif
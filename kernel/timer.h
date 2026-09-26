#ifndef TIMER_H
#define TIMER_H

#define CLINT_MTIMECMP 0x02004000
#define CLINT_MTIME 0x0200BFF8

#define TIMER_FREQ_HZ 10000000
#define TIMER_TICK_HZ 100
#define TIMER_INTERVAL (TIMER_FREQ_HZ / TIMER_TICK_HZ)

#ifndef __ASSEMBLER__

#include <stdint.h>

uint64_t timer_read(void);
void timer_interrupt(void);
void timer_tick(uint64_t count);
uint64_t timer_ticks(void);

#endif

#endif

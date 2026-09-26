#ifndef RTC_H
#define RTC_H

#include <stdint.h>

#define RTC_BASE 0x00101000UL

void rtc_register(void);
uint64_t rtc_seconds(void);

#endif

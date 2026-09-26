#ifndef _KNOC_TIME_H
#define _KNOC_TIME_H

#include <stddef.h>

#define CLOCKS_PER_SEC 100

typedef long time_t;
typedef long clock_t;

struct tm
{
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

time_t time(time_t *result);
clock_t clock(void);
double difftime(time_t end, time_t start);
struct tm *gmtime(const time_t *value);
struct tm *localtime(const time_t *value);
size_t strftime(char *buffer, size_t size, const char *format, const struct tm *value);

#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include "../ulib.h"

#define ARGV_MAX 32

int errno;

static struct tm broken;

time_t time(time_t *result)
{
    time_t now = (time_t)(uptime() / 100);

    if (result)
    {
        *result = now;
    }

    return now;
}

clock_t clock(void)
{
    return (clock_t)uptime();
}

double difftime(time_t end, time_t start)
{
    return (double)(end - start);
}

struct tm *gmtime(const time_t *value)
{
    long seconds = *value;
    long days = seconds / 86400;

    memset(&broken, 0, sizeof(broken));
    broken.tm_sec = (int)(seconds % 60);
    broken.tm_min = (int)((seconds / 60) % 60);
    broken.tm_hour = (int)((seconds / 3600) % 24);
    broken.tm_wday = (int)((days + 4) % 7);

    int year = 1970;

    while (1)
    {
        int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        int length = leap ? 366 : 365;

        if (days < length)
        {
            break;
        }

        days -= length;
        year++;
    }

    static const int month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    int month = 0;

    broken.tm_yday = (int)days;

    while (month < 11 && days >= month_days[month] + (month == 1 && leap))
    {
        days -= month_days[month] + (month == 1 && leap);
        month++;
    }

    broken.tm_year = year - 1900;
    broken.tm_mon = month;
    broken.tm_mday = (int)days + 1;
    return &broken;
}

struct tm *localtime(const time_t *value)
{
    return gmtime(value);
}

size_t strftime(char *buffer, size_t size, const char *format, const struct tm *value)
{
    static const char *days[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    size_t n = 0;

    for (; *format && n + 1 < size; format++)
    {
        char part[32];
        int length;

        if (*format != '%')
        {
            buffer[n++] = *format;
            continue;
        }

        format++;

        switch (*format)
        {
        case 'Y':
            length = snprintf(part, sizeof(part), "%04d", value->tm_year + 1900);
            break;
        case 'm':
            length = snprintf(part, sizeof(part), "%02d", value->tm_mon + 1);
            break;
        case 'd':
            length = snprintf(part, sizeof(part), "%02d", value->tm_mday);
            break;
        case 'H':
            length = snprintf(part, sizeof(part), "%02d", value->tm_hour);
            break;
        case 'M':
            length = snprintf(part, sizeof(part), "%02d", value->tm_min);
            break;
        case 'S':
            length = snprintf(part, sizeof(part), "%02d", value->tm_sec);
            break;
        case 'j':
            length = snprintf(part, sizeof(part), "%03d", value->tm_yday + 1);
            break;
        case 'a':
            length = snprintf(part, sizeof(part), "%s", days[value->tm_wday % 7]);
            break;
        case 'b':
            length = snprintf(part, sizeof(part), "%s", months[value->tm_mon % 12]);
            break;
        case '%':
            length = snprintf(part, sizeof(part), "%%");
            break;
        default:
            length = snprintf(part, sizeof(part), "%%%c", *format ? *format : ' ');
            break;
        }

        if (*format == 0 || n + (size_t)length >= size)
        {
            break;
        }

        memcpy(buffer + n, part, (size_t)length);
        n += (size_t)length;
    }

    if (size > 0)
    {
        buffer[n < size ? n : size - 1] = 0;
    }

    return n;
}

void __knoc_assert_fail(const char *expression, const char *file, int line)
{
    fprintf(stderr, "assertion failed: %s (%s:%d)\n", expression, file, line);
    abort();
}

int main(int argc, char **argv);

__attribute__((section(".text.start"))) void _start(void)
{
    static char args[ARGS_MAX];
    static char name[] = "program";
    static char *argv[ARGV_MAX + 1];
    int argc = 0;

    argv[argc++] = name;
    getargs(args, sizeof(args));

    for (char *p = args; *p && argc < ARGV_MAX;)
    {
        while (*p == ' ')
        {
            *p++ = 0;
        }

        if (!*p)
        {
            break;
        }

        argv[argc++] = p;

        while (*p && *p != ' ')
        {
            p++;
        }
    }

    argv[argc] = NULL;
    exit(main(argc, argv));
}

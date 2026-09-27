#include "rtc.h"
#include "mmio.h"
#include "device.h"

#define RTC_TIME_LOW 0x00
#define RTC_TIME_HIGH 0x04
#define NANOSECONDS 1000000000UL
#define YEAR_2020 1577836800UL

static int ready;

static uint64_t rtc_nanoseconds(void)
{
    uint32_t low = *(volatile uint32_t *)MMIO(RTC_BASE + RTC_TIME_LOW);
    uint32_t high = *(volatile uint32_t *)MMIO(RTC_BASE + RTC_TIME_HIGH);

    return ((uint64_t)high << 32) | low;
}

uint64_t rtc_seconds(void)
{
    return ready ? rtc_nanoseconds() / NANOSECONDS : 0;
}

static int rtc_init(device_t *dev)
{
    (void)dev;

    if (rtc_nanoseconds() / NANOSECONDS < YEAR_2020)
    {
        return -1;
    }

    ready = 1;
    return 0;
}

static int64_t rtc_read(device_t *dev, void *buffer, uint64_t length)
{
    (void)dev;

    if (length < sizeof(uint64_t))
    {
        return -1;
    }

    *(uint64_t *)buffer = rtc_seconds();
    return sizeof(uint64_t);
}

static device_t rtc_device = {
    .name = "rtc0",
    .irq = DEVICE_NO_IRQ,
    .init = rtc_init,
    .read = rtc_read,
};

void rtc_register(void)
{
    device_register(&rtc_device);
}

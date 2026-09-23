#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>

#define DEVICE_MAX 16
#define DEVICE_NO_IRQ 0

typedef struct device
{
    const char *name;
    uint32_t irq;
    int (*init)(struct device *dev);
    void (*interrupt)(struct device *dev);
    int64_t (*read)(struct device *dev, void *buffer, uint64_t length);
    int64_t (*write)(struct device *dev, const void *buffer, uint64_t length);
    int ready;
} device_t;

int device_register(device_t *dev);
void device_init_all(void);
device_t *device_find(const char *name);
uint32_t device_count(void);

int64_t device_read(device_t *dev, void *buffer, uint64_t length);
int64_t device_write(device_t *dev, const void *buffer, uint64_t length);
int device_handle_irq(uint32_t irq);

void device_list(void);

#endif

#ifndef VIRTIO_RNG_H
#define VIRTIO_RNG_H

#include <stdint.h>

void virtio_rng_register(void);
int64_t virtio_rng_read(void *buffer, uint64_t length);

#endif

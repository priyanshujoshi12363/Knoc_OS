#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>

#define VIRTIO_BLK_SECTOR_SIZE 512
#define VIRTIO_BLK_MAX_SECTORS 256

void virtio_blk_register(void);

void virtio_blk_stats(uint64_t *reads, uint64_t *writes, uint64_t *wait_ticks);

#endif

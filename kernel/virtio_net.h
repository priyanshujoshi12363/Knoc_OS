#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>

void virtio_net_register(void);
int virtio_net_send(const void *frame, uint32_t length);
int virtio_net_ready(void);

#endif

#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdint.h>

#define GPU_CURSOR_SIZE 64

void virtio_gpu_register(void);
int gpu_ready(void);
uint32_t *gpu_framebuffer(void);
uint32_t gpu_width(void);
uint32_t gpu_height(void);
uint64_t gpu_framebuffer_bytes(void);
int gpu_flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
int gpu_cursor_set(const uint32_t *pixels, uint32_t hot_x, uint32_t hot_y);
int gpu_cursor_move(uint32_t x, uint32_t y);
int gpu_cursor_hide(void);

#endif

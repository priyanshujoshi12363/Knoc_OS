#ifndef MMIO_H
#define MMIO_H

#include <stdint.h>

#define MMIO_ALIAS 0xFFFFFFFFC0000000UL
#define MMIO_SLOT 511

extern uintptr_t mmio_offset;

#define MMIO(address) (mmio_offset + (uintptr_t)(address))

#endif

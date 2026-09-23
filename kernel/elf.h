#ifndef ELF_H
#define ELF_H

#include <stdint.h>

/* Called for each loadable segment: returns zeroed memory of at least
   `bytes` bytes, or 0 when the program is out of memory */
typedef void *(*elf_alloc_t)(void *context, uint64_t bytes);

int elf_load(uintptr_t root,
             const uint8_t *image,
             uint64_t size,
             elf_alloc_t alloc,
             void *context,
             uintptr_t *entry);

#endif

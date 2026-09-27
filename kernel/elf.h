#ifndef ELF_H
#define ELF_H

#include <stdint.h>

/* Called for each loadable segment: returns zeroed memory of at least
   `bytes` bytes, or 0 when the program is out of memory */
typedef void *(*elf_alloc_t)(void *context, uint64_t bytes);

typedef struct elf_linux_info
{
    uintptr_t entry;
    uintptr_t phdr;
    uintptr_t brk;
    uintptr_t base;
    uint16_t phnum;
    uint16_t phent;
} elf_linux_info_t;

#define ELF_LINUX_DYNAMIC -2

int elf_is_linux(const uint8_t *image, uint64_t size);
int elf_load_linux(uintptr_t root, const uint8_t *image, uint64_t size, elf_alloc_t alloc, void *context,
                   elf_linux_info_t *info);

int elf_load(uintptr_t root,
             const uint8_t *image,
             uint64_t size,
             elf_alloc_t alloc,
             void *context,
             uintptr_t *entry);

#endif

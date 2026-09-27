#ifndef ELF_H
#define ELF_H

#include <stdint.h>

/* Called for each loadable segment: returns zeroed memory of at least
   `bytes` bytes, or 0 when the program is out of memory */
typedef void *(*elf_alloc_t)(void *context, uint64_t bytes);

typedef uint8_t *(*elf_page_t)(void *context, uintptr_t address, uint64_t rwx);

#define ELF_INTERP_MAX 128

typedef struct elf_linux_info
{
    uintptr_t entry;
    uintptr_t phdr;
    uintptr_t brk;
    uintptr_t base;
    uint16_t phnum;
    uint16_t phent;
    uintptr_t interp_base;
    char interp[ELF_INTERP_MAX];
} elf_linux_info_t;

int elf_is_linux(const uint8_t *image, uint64_t size);
int elf_load_linux(const uint8_t *image, uint64_t size, uintptr_t dyn_base, elf_page_t page, void *context,
                   elf_linux_info_t *info);

int elf_load(uintptr_t root,
             const uint8_t *image,
             uint64_t size,
             elf_alloc_t alloc,
             void *context,
             uintptr_t *entry);

#endif

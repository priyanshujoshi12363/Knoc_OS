#ifndef FDT_H
#define FDT_H

#include <stdint.h>

typedef struct fdt_info
{
    uintptr_t dtb_start;
    uint64_t dtb_size;
    uintptr_t ram_start;
    uint64_t ram_size;
    uint32_t cpu_count;
} fdt_info_t;

int fdt_parse(uintptr_t dtb, fdt_info_t *info);

#endif

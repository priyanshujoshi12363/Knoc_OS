#ifndef PAGE_H
#define PAGE_H

#include <stdint.h>

#define PAGE_SIZE 4096UL
#define PAGE_MAX_ORDER 18

int page_init(uintptr_t ram_start,
              uint64_t ram_size,
              uintptr_t dtb_start,
              uint64_t dtb_size);

void page_debug(void);
void *page_alloc(void);
void *page_alloc_order(unsigned int order);
void *page_alloc_contiguous(uint64_t bytes);
void page_free(void *address);

unsigned long page_total(void);
unsigned long page_used(void);
unsigned long page_free_count(void);
unsigned long page_largest_free(void);
uintptr_t page_ram_start(void);
uintptr_t page_ram_end(void);

#endif

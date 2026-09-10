#ifndef PAGE_H
#define PAGE_H

#define PAGE_SIZE 4096UL

void page_init(void);
void page_debug(void);
void *page_alloc(void);
void page_free(void *address);

#endif
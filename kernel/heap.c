#include "heap.h"
#include "page.h"
#include "vm.h"

#define HEAP_START 0x90000000UL

typedef struct heap_block
{
    uint64_t size;
    uint64_t free;
    struct heap_block *next;
} heap_block_t;

static heap_block_t *heap_first_block;
static uintptr_t heap_physical_page;

void heap_init(void)
{
    heap_physical_page = (uintptr_t)page_alloc();

    if (heap_physical_page == 0)
    {
        heap_first_block = 0;
        return;
    }

    if (vm_map(HEAP_START,
               heap_physical_page,
               PTE_R | PTE_W) != 0)
    {
        page_free((void *)heap_physical_page);
        heap_physical_page = 0;
        heap_first_block = 0;
        return;
    }

    heap_first_block = (heap_block_t *)HEAP_START;
}

void heap_activate(void)
{
    if (heap_physical_page == 0)
    {
        heap_first_block = 0;
        return;
    }

    heap_first_block->size =
        PAGE_SIZE - sizeof(heap_block_t);

    heap_first_block->free = 1;
    heap_first_block->next = 0;
}

void *kmalloc(uint64_t size)
{
    heap_block_t *block = heap_first_block;

    if (size == 0)
    {
        return 0;
    }

    while (block != 0)
    {
        if (block->free && block->size >= size)
        {
            block->free = 0;

            return (void *)(block + 1);
        }

        block = block->next;
    }

    return 0;
}

void kfree(void *address)
{
    if (address == 0)
    {
        return;
    }

    heap_block_t *block =
        ((heap_block_t *)address) - 1;

    block->free = 1;
}
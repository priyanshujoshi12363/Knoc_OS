#include "heap.h"
#include "page.h"
#include "vm.h"
#include "spinlock.h"

/* Far above RAM, which is identity-mapped from 0x80000000 */
#define HEAP_START 0x2000000000UL
#define HEAP_MAX (HEAP_START + 0x10000000UL)
#define HEAP_ALIGNMENT 8UL

typedef struct heap_block
{
    uint64_t size;
    uint64_t free;
    struct heap_block *next;
} heap_block_t;

static heap_block_t *heap_first_block;
static heap_block_t *heap_last_block;
static uintptr_t heap_physical_page;
static uintptr_t heap_end;
static spinlock_t heap_lock = SPINLOCK_INIT;

static uint64_t align_size(uint64_t size)
{
    if (size > UINT64_MAX - (HEAP_ALIGNMENT - 1))
    {
        return 0;
    }

    return (size + HEAP_ALIGNMENT - 1) &
           ~(HEAP_ALIGNMENT - 1);
}

static void split_block(heap_block_t *block, uint64_t size)
{
    if (block->size < size + sizeof(heap_block_t) + HEAP_ALIGNMENT)
    {
        return;
    }

    heap_block_t *new_block =
        (heap_block_t *)((uintptr_t)(block + 1) + size);

    new_block->size =
        block->size - size - sizeof(heap_block_t);

    new_block->free = 1;
    new_block->next = block->next;

    block->size = size;
    block->next = new_block;

    if (heap_last_block == block)
    {
        heap_last_block = new_block;
    }
}

static int block_belongs_to_heap(heap_block_t *target)
{
    heap_block_t *block = heap_first_block;

    while (block != 0)
    {
        if (block == target)
        {
            return 1;
        }

        block = block->next;
    }

    return 0;
}

static void merge_with_next(heap_block_t *block)
{
    heap_block_t *next = block->next;

    if (next == 0 || !next->free)
    {
        return;
    }

    block->size += sizeof(heap_block_t) + next->size;
    block->next = next->next;

    if (heap_last_block == next)
    {
        heap_last_block = block;
    }
}

static void coalesce_blocks(void)
{
    heap_block_t *block = heap_first_block;

    while (block != 0 && block->next != 0)
    {
        if (block->free && block->next->free)
        {
            merge_with_next(block);
        }
        else
        {
            block = block->next;
        }
    }
}

static int heap_grow(void)
{
    if (heap_end >= HEAP_MAX)
    {
        return -1;
    }

    uintptr_t physical_page =
        (uintptr_t)page_alloc();

    if (physical_page == 0)
    {
        return -1;
    }

    if (vm_map(heap_end,
               physical_page,
               PTE_R | PTE_W) != 0)
    {
        page_free((void *)physical_page);
        return -1;
    }

    asm volatile("sfence.vma zero, zero");

    uintptr_t old_heap_end = heap_end;

    heap_end += PAGE_SIZE;

    if (heap_last_block != 0 &&
        heap_last_block->free)
    {
        heap_last_block->size += PAGE_SIZE;
    }
    else
    {
        heap_block_t *new_block =
            (heap_block_t *)old_heap_end;

        new_block->size =
            PAGE_SIZE - sizeof(heap_block_t);

        new_block->free = 1;
        new_block->next = 0;

        if (heap_first_block == 0)
        {
            heap_first_block = new_block;
        }

        if (heap_last_block != 0)
        {
            heap_last_block->next = new_block;
        }

        heap_last_block = new_block;
    }

    return 0;
}

void heap_init(void)
{
    heap_physical_page =
        (uintptr_t)page_alloc();

    if (heap_physical_page == 0)
    {
        heap_first_block = 0;
        heap_last_block = 0;
        heap_end = 0;
        return;
    }

    if (vm_map(HEAP_START,
               heap_physical_page,
               PTE_R | PTE_W) != 0)
    {
        page_free((void *)heap_physical_page);
        heap_physical_page = 0;
        heap_first_block = 0;
        heap_last_block = 0;
        heap_end = 0;
        return;
    }

    heap_end = HEAP_START + PAGE_SIZE;
    heap_first_block = 0;
    heap_last_block = 0;
}

void heap_activate(void)
{
    if (heap_physical_page == 0)
    {
        heap_first_block = 0;
        heap_last_block = 0;
        return;
    }

    heap_first_block =
        (heap_block_t *)HEAP_START;

    heap_first_block->size =
        PAGE_SIZE - sizeof(heap_block_t);

    heap_first_block->free = 1;
    heap_first_block->next = 0;

    heap_last_block = heap_first_block;
}

static void *kmalloc_locked(uint64_t size)
{
    if (size == 0 || heap_first_block == 0)
    {
        return 0;
    }

    size = align_size(size);

    if (size == 0)
    {
        return 0;
    }

    while (1)
    {
        heap_block_t *block = heap_first_block;

        while (block != 0)
        {
            if (block->free && block->size >= size)
            {
                split_block(block, size);

                block->free = 0;

                return (void *)(block + 1);
            }

            block = block->next;
        }

        if (heap_grow() != 0)
        {
            return 0;
        }
    }
}

static void kfree_locked(void *address)
{
    if (address == 0 || heap_first_block == 0)
    {
        return;
    }

    heap_block_t *block =
        ((heap_block_t *)address) - 1;

    if (!block_belongs_to_heap(block))
    {
        return;
    }

    if (block->free)
    {
        return;
    }

    block->free = 1;

    coalesce_blocks();
}
void *kmalloc(uint64_t size)
{
    uint64_t interrupts = spin_lock(&heap_lock);
    void *address = kmalloc_locked(size);
    spin_unlock(&heap_lock, interrupts);

    return address;
}

void kfree(void *address)
{
    uint64_t interrupts = spin_lock(&heap_lock);
    kfree_locked(address);
    spin_unlock(&heap_lock, interrupts);
}

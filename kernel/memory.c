#include <stdint.h>

#define BYTES_PER_KB 1024UL
#define BYTES_PER_MB (1024UL * 1024UL)
#define BYTES_PER_GB (1024UL * 1024UL * 1024UL)


extern char kernel_start;
extern char kernel_end;
extern char stack_bottom;
extern char stack_top;

#define RAM_START 0x80000000UL
#define RAM_END   0x88000000UL

void memory_init(void)
{
    uintptr_t kernel_start_addr = (uintptr_t)&kernel_start;
    uintptr_t kernel_end_addr   = (uintptr_t)&kernel_end;

    uintptr_t stack_start_addr  = (uintptr_t)&stack_bottom;
    uintptr_t stack_end_addr    = (uintptr_t)&stack_top;

    uintptr_t free_start = stack_end_addr;
    uintptr_t free_end   = RAM_END;

    (void)kernel_start_addr;
    (void)kernel_end_addr;
    (void)stack_start_addr;
    (void)free_start;
    (void)free_end;
}


uint64_t memory_total_bytes(void)
{
    return RAM_END - RAM_START;
}

uint64_t memory_total_kb(void)
{
    return memory_total_bytes() / BYTES_PER_KB;
}


uint64_t memory_total_mb(void)
{
    return memory_total_bytes() / BYTES_PER_MB;
}


uint64_t memory_total_gb(void)
{
    return memory_total_bytes() / BYTES_PER_GB;
}

uint64_t memory_kernel_bytes(void)
{
    return (uintptr_t)&kernel_end - (uintptr_t)&kernel_start;
}

uint64_t memory_stack_bytes(void)
{
    return (uintptr_t)&stack_top - (uintptr_t)&stack_bottom;
}


uint64_t memory_used_bytes(void)
{
    return memory_kernel_bytes() + memory_stack_bytes();
}


uint64_t memory_free_bytes(void)
{
    return memory_total_bytes() - memory_used_bytes();
}


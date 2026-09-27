#ifndef VM_H
#define VM_H

#include <stdint.h>

#define VM_PAGE_SIZE 4096UL
#define VM_MEGAPAGE_SIZE (2UL * 1024 * 1024)

#define VM_UART      0x10000000UL

#define PTE_V (1UL << 0)
#define PTE_R (1UL << 1)
#define PTE_W (1UL << 2)
#define PTE_X (1UL << 3)
#define PTE_U (1UL << 4)

#define PA_TO_PPN(pa) ((pa) >> 12)
#define PPN_TO_PA(ppn) ((ppn) << 12)

#define VPN_MASK 0x1FFUL

#define VA_VPN0(va) (((va) >> 12) & VPN_MASK)
#define VA_VPN1(va) (((va) >> 21) & VPN_MASK)
#define VA_VPN2(va) (((va) >> 30) & VPN_MASK)

typedef uint64_t pte_t;

typedef pte_t page_table_t[512];

pte_t vm_make_pte(uintptr_t physical_address, uint64_t flags);

#define PTE_OWNED (1UL << 8)
#define PTE_SLOT (1UL << 9)

int vm_page_set(uintptr_t root, uintptr_t virtual_address, uintptr_t physical_address, uint64_t flags);
int vm_page_get(uintptr_t root, uintptr_t virtual_address, pte_t *entry);
int vm_page_clear(uintptr_t root, uintptr_t virtual_address, pte_t *old);
int vm_page_protect(uintptr_t root, uintptr_t virtual_address, uint64_t rwx);

void vm_init(uintptr_t ram_start, uintptr_t ram_end);
void vm_enable(void);
uint64_t vm_megapage_count(void);

uint64_t vm_make_satp(uintptr_t root);
uint64_t vm_kernel_satp(void);
void vm_switch(uint64_t satp);

uintptr_t vm_user_create(void);
int vm_user_map(uintptr_t root,
                uintptr_t virtual_address,
                uintptr_t physical_address,
                uint64_t size,
                uint64_t flags);
int vm_user_translate(uintptr_t root,
                      uintptr_t virtual_address,
                      uint64_t need,
                      uintptr_t *physical_address);
void vm_user_destroy(uintptr_t root);

int vm_map(uintptr_t virtual_address,
           uintptr_t physical_address,
           uint64_t flags);

int vm_map_range(uintptr_t virtual_start,
                 uintptr_t physical_start,
                 uintptr_t size,
                 uint64_t flags);

void vm_debug(uintptr_t virtual_address);
int vm_map_test(uintptr_t virtual_address,
                uintptr_t physical_address,
                uint64_t flags);

#endif
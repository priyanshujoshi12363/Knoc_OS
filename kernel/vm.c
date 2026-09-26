#include "vm.h"
#include "page.h"
#include "uart.h"
#include "plic.h"
#include "power.h"
#include "virtio.h"
#include "rtc.h"
#include "syscall_abi.h"

static page_table_t *root_page_table;
static uint64_t megapages_mapped;

#define PTE_LEAF (PTE_R | PTE_W | PTE_X)

static void vm_clear_page_table(page_table_t *page_table)
{
    for (unsigned long i = 0; i < 512; i++)
    {
        (*page_table)[i] = 0;
    }
}

pte_t vm_make_pte(uintptr_t physical_address, uint64_t flags)
{
    return (PA_TO_PPN(physical_address) << 10) | flags;
}

void vm_enable(void)
{
    uintptr_t root_address = (uintptr_t)root_page_table;
    uint64_t satp_value = (8ULL << 60) | (root_address >> 12);

    asm volatile("csrw satp, %0" :: "r"(satp_value));
    asm volatile("sfence.vma zero, zero");
}
uint64_t vm_megapage_count(void)
{
    return megapages_mapped;
}

void vm_init(uintptr_t ram_start, uintptr_t ram_end)
{
    root_page_table = (page_table_t *)page_alloc();

    if (root_page_table == 0)
    {
        return;
    }

    vm_clear_page_table(root_page_table);

    if (vm_map_range(ram_start,
                     ram_start,
                     ram_end - ram_start,
                     PTE_R | PTE_W | PTE_X) != 0)
    {
        root_page_table = 0;
        return;
    }

    if (vm_map_range(VM_UART,
                     VM_UART,
                     VM_PAGE_SIZE,
                     PTE_R | PTE_W) != 0)
    {
        root_page_table = 0;
        return;
    }

    if (vm_map_range(POWER_BASE,
                     POWER_BASE,
                     VM_PAGE_SIZE,
                     PTE_R | PTE_W) != 0)
    {
        root_page_table = 0;
        return;
    }

    if (vm_map_range(RTC_BASE,
                     RTC_BASE,
                     VM_PAGE_SIZE,
                     PTE_R | PTE_W) != 0)
    {
        root_page_table = 0;
        return;
    }

    if (vm_map_range(VIRTIO0_BASE,
                     VIRTIO0_BASE,
                     VM_PAGE_SIZE * 8,
                     PTE_R | PTE_W) != 0)
    {
        root_page_table = 0;
        return;
    }

    if (vm_map_range(PLIC_BASE,
                     PLIC_BASE,
                     PLIC_SIZE,
                     PTE_R | PTE_W) != 0)
    {
        root_page_table = 0;
        return;
    }
}

static page_table_t *vm_level1(page_table_t *root, uintptr_t virtual_address)
{
    unsigned long vpn2 = VA_VPN2(virtual_address);

    if (!((*root)[vpn2] & PTE_V))
    {
        page_table_t *level1 = (page_table_t *)page_alloc();

        if (level1 == 0)
        {
            return 0;
        }

        vm_clear_page_table(level1);

        (*root)[vpn2] =
            vm_make_pte((uintptr_t)level1, PTE_V);

        return level1;
    }

    if ((*root)[vpn2] & PTE_LEAF)
    {
        return 0;
    }

    return (page_table_t *)PPN_TO_PA((*root)[vpn2] >> 10);
}

/* A megapage is a leaf entry in the level-1 table: one entry maps
   2 MiB directly, with no level-0 table below it */
static int vm_map_mega(page_table_t *root,
                       uintptr_t virtual_address,
                       uintptr_t physical_address,
                       uint64_t flags)
{
    if (root == 0 ||
        (virtual_address | physical_address) & (VM_MEGAPAGE_SIZE - 1))
    {
        return -1;
    }

    page_table_t *level1 = vm_level1(root, virtual_address);

    if (level1 == 0)
    {
        return -1;
    }

    unsigned long vpn1 = VA_VPN1(virtual_address);

    if ((*level1)[vpn1] & PTE_V)
    {
        return -1;
    }

    (*level1)[vpn1] = vm_make_pte(physical_address, flags | PTE_V);

    if (root == root_page_table)
    {
        megapages_mapped++;
    }

    return 0;
}

static int vm_map_page(page_table_t *root,
                       uintptr_t virtual_address,
                       uintptr_t physical_address,
                       uint64_t flags)
{
    unsigned long vpn1 = VA_VPN1(virtual_address);
    unsigned long vpn0 = VA_VPN0(virtual_address);

    page_table_t *level1;
    page_table_t *level0;

    if (root == 0)
    {
        return -1;
    }

    level1 = vm_level1(root, virtual_address);

    if (level1 == 0)
    {
        return -1;
    }

    if ((*level1)[vpn1] & PTE_LEAF)
    {
        return -1;
    }

    if (!((*level1)[vpn1] & PTE_V))
    {
        level0 = (page_table_t *)page_alloc();

        if (level0 == 0)
        {
            return -1;
        }

        vm_clear_page_table(level0);

        (*level1)[vpn1] =
            vm_make_pte((uintptr_t)level0, PTE_V);
    }
    else
    {
        level0 = (page_table_t *)
            PPN_TO_PA((*level1)[vpn1] >> 10);
    }

    (*level0)[vpn0] =
        vm_make_pte(physical_address, flags | PTE_V);

    return 0;
}

static int vm_map_range_in(page_table_t *root,
                           uintptr_t virtual_start,
                           uintptr_t physical_start,
                           uintptr_t size,
                           uint64_t flags)
{
    uintptr_t offset = 0;

    while (offset < size)
    {
        uintptr_t virtual_address = virtual_start + offset;
        uintptr_t physical_address = physical_start + offset;

        if (((virtual_address | physical_address) & (VM_MEGAPAGE_SIZE - 1)) == 0 &&
            size - offset >= VM_MEGAPAGE_SIZE)
        {
            if (vm_map_mega(root, virtual_address, physical_address, flags) != 0)
            {
                return -1;
            }

            offset += VM_MEGAPAGE_SIZE;
            continue;
        }

        if (vm_map_page(root, virtual_address, physical_address, flags) != 0)
        {
            return -1;
        }

        offset += VM_PAGE_SIZE;
    }

    return 0;
}

int vm_map(uintptr_t virtual_address,
           uintptr_t physical_address,
           uint64_t flags)
{
    return vm_map_page(root_page_table, virtual_address, physical_address, flags);
}

int vm_map_range(uintptr_t virtual_start,
                 uintptr_t physical_start,
                 uintptr_t size,
                 uint64_t flags)
{
    return vm_map_range_in(root_page_table, virtual_start, physical_start, size, flags);
}

/* User address spaces. Each program gets its own root table: a copy of
   the kernel's root entries (so the kernel stays mapped while it handles
   a trap, but without the U bit, so the program can't touch it), plus
   private tables for the user slots (USER_BASE..USER_END). */

uint64_t vm_make_satp(uintptr_t root)
{
    return (8ULL << 60) | (root >> 12);
}

uint64_t vm_kernel_satp(void)
{
    return vm_make_satp((uintptr_t)root_page_table);
}

void vm_switch(uint64_t satp)
{
    uint64_t current;

    asm volatile("csrr %0, satp" : "=r"(current));

    if (current != satp)
    {
        asm volatile("csrw satp, %0" :: "r"(satp));
        asm volatile("sfence.vma zero, zero");
    }
}

uintptr_t vm_user_create(void)
{
    page_table_t *root = (page_table_t *)page_alloc();

    if (root == 0)
    {
        return 0;
    }

    for (unsigned long i = 0; i < 512; i++)
    {
        (*root)[i] = (*root_page_table)[i];
    }

    for (unsigned long i = VA_VPN2(USER_BASE); i < VA_VPN2(USER_END); i++)
    {
        (*root)[i] = 0;
    }

    return (uintptr_t)root;
}

static int user_range_valid(uintptr_t virtual_address, uint64_t size)
{
    return virtual_address >= USER_BASE &&
           virtual_address < USER_END &&
           size <= USER_END - virtual_address;
}

int vm_user_map(uintptr_t root,
                uintptr_t virtual_address,
                uintptr_t physical_address,
                uint64_t size,
                uint64_t flags)
{
    if (!user_range_valid(virtual_address, size))
    {
        return -1;
    }

    return vm_map_range_in((page_table_t *)root,
                           virtual_address,
                           physical_address,
                           size,
                           flags | PTE_U);
}

int vm_user_translate(uintptr_t root,
                      uintptr_t virtual_address,
                      uint64_t need,
                      uintptr_t *physical_address)
{
    if (!user_range_valid(virtual_address, 1))
    {
        return -1;
    }

    pte_t entry = (*(page_table_t *)root)[VA_VPN2(virtual_address)];

    if (!(entry & PTE_V) || (entry & PTE_LEAF))
    {
        return -1;
    }

    entry = (*(page_table_t *)PPN_TO_PA(entry >> 10))[VA_VPN1(virtual_address)];

    if (!(entry & PTE_V))
    {
        return -1;
    }

    uintptr_t offset_mask = VM_MEGAPAGE_SIZE - 1;

    if (!(entry & PTE_LEAF))
    {
        entry = (*(page_table_t *)PPN_TO_PA(entry >> 10))[VA_VPN0(virtual_address)];
        offset_mask = VM_PAGE_SIZE - 1;
    }

    if (!(entry & PTE_V) || !(entry & PTE_U) || (entry & need) != need)
    {
        return -1;
    }

    *physical_address = PPN_TO_PA(entry >> 10) + (virtual_address & offset_mask);
    return 0;
}

void vm_user_destroy(uintptr_t root)
{
    page_table_t *table = (page_table_t *)root;

    for (unsigned long i = VA_VPN2(USER_BASE); i < VA_VPN2(USER_END); i++)
    {
        pte_t entry = (*table)[i];

        if (!(entry & PTE_V) || (entry & PTE_LEAF))
        {
            continue;
        }

        page_table_t *level1 = (page_table_t *)PPN_TO_PA(entry >> 10);

        for (unsigned long j = 0; j < 512; j++)
        {
            pte_t level1_entry = (*level1)[j];

            if ((level1_entry & PTE_V) && !(level1_entry & PTE_LEAF))
            {
                page_free((void *)PPN_TO_PA(level1_entry >> 10));
            }
        }

        page_free(level1);
    }

    page_free(table);
}

void vm_debug(uintptr_t virtual_address)
{
    unsigned long vpn2 = VA_VPN2(virtual_address);
    unsigned long vpn1 = VA_VPN1(virtual_address);
    unsigned long vpn0 = VA_VPN0(virtual_address);

    pte_t root_pte;
    pte_t level1_pte;
    pte_t level0_pte;

    uart_puts("\n");
    uart_puts("================================\n");
    uart_puts("        Sv39 Mapping Debug\n");
    uart_puts("================================\n");

    uart_puts("Virtual Address : ");
    uart_put_hex(virtual_address);
    uart_puts("\n");

    uart_puts("VPN[2]          : ");
    uart_put_uint(vpn2);
    uart_puts("\n");

    uart_puts("VPN[1]          : ");
    uart_put_uint(vpn1);
    uart_puts("\n");

    uart_puts("VPN[0]          : ");
    uart_put_uint(vpn0);
    uart_puts("\n");

    if (root_page_table == 0)
    {
        uart_puts("Root Page Table : NOT INITIALIZED\n");
        return;
    }

    root_pte = (*root_page_table)[vpn2];

    uart_puts("Root PTE        : ");
    uart_put_hex(root_pte);
    uart_puts("\n");

    if (!(root_pte & PTE_V))
    {
        uart_puts("Mapping Status  : ROOT ENTRY INVALID\n");
        return;
    }

    page_table_t *level1 =
        (page_table_t *)PPN_TO_PA(root_pte >> 10);

    level1_pte = (*level1)[vpn1];

    uart_puts("Level-1 PTE     : ");
    uart_put_hex(level1_pte);
    uart_puts("\n");

    if (!(level1_pte & PTE_V))
    {
        uart_puts("Mapping Status  : LEVEL-1 ENTRY INVALID\n");
        return;
    }

    if (level1_pte & PTE_LEAF)
    {
        uart_puts("Physical Address: ");
        uart_put_hex(PPN_TO_PA(level1_pte >> 10) + (virtual_address & (VM_MEGAPAGE_SIZE - 1)));
        uart_puts("\n");
        uart_puts("Mapping Status  : VALID (2 MiB megapage)\n");
        uart_puts("================================\n");
        return;
    }

    page_table_t *level0 =
        (page_table_t *)PPN_TO_PA(level1_pte >> 10);

    level0_pte = (*level0)[vpn0];

    uart_puts("Level-0 PTE     : ");
    uart_put_hex(level0_pte);
    uart_puts("\n");

    if (!(level0_pte & PTE_V))
    {
        uart_puts("Mapping Status  : FINAL ENTRY INVALID\n");
        return;
    }

    uart_puts("Physical Address: ");
    uart_put_hex(PPN_TO_PA(level0_pte >> 10));
    uart_puts("\n");

    uart_puts("Mapping Status  : VALID\n");
    uart_puts("================================\n");
}
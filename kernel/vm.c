#include "vm.h"
#include "page.h"
#include "uart.h"
#include "plic.h"

static page_table_t *root_page_table;

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
void vm_init(void)
{
    root_page_table = (page_table_t *)page_alloc();

    if (root_page_table == 0)
    {
        return;
    }

    vm_clear_page_table(root_page_table);

    if (vm_map_range(VM_RAM_START,
                     VM_RAM_START,
                     VM_RAM_END - VM_RAM_START,
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

    if (vm_map_range(PLIC_BASE,
                     PLIC_BASE,
                     PLIC_SIZE,
                     PTE_R | PTE_W) != 0)
    {
        root_page_table = 0;
        return;
    }
}

int vm_map(uintptr_t virtual_address,
           uintptr_t physical_address,
           uint64_t flags)
{
    unsigned long vpn2 = VA_VPN2(virtual_address);
    unsigned long vpn1 = VA_VPN1(virtual_address);
    unsigned long vpn0 = VA_VPN0(virtual_address);

    page_table_t *level1;
    page_table_t *level0;

    if (root_page_table == 0)
    {
        return -1;
    }

    if (!((*root_page_table)[vpn2] & PTE_V))
    {
        level1 = (page_table_t *)page_alloc();

        if (level1 == 0)
        {
            return -1;
        }

        vm_clear_page_table(level1);

        (*root_page_table)[vpn2] =
            vm_make_pte((uintptr_t)level1, PTE_V);
    }
    else
    {
        level1 = (page_table_t *)
            PPN_TO_PA((*root_page_table)[vpn2] >> 10);
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
int vm_map_range(uintptr_t virtual_start,
                 uintptr_t physical_start,
                 uintptr_t size,
                 uint64_t flags)
{
    for (uintptr_t offset = 0; offset < size; offset += VM_PAGE_SIZE)
    {
        if (vm_map(virtual_start + offset,
                   physical_start + offset,
                   flags) != 0)
        {
            return -1;
        }
    }

    return 0;
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
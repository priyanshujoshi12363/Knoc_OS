#include "elf.h"
#include "vm.h"
#include "page.h"
#include "string.h"
#include "syscall_abi.h"

/* ELF is the file format of Linux programs (and of knocos.elf itself).
   The program headers say which bytes of the file go where in memory
   (PT_LOAD segments) and with which permissions. */

#define ELF_MAGIC 0x464C457FU
#define ELF_CLASS_64 2
#define ELF_TYPE_EXEC 2
#define ELF_TYPE_DYN 3
#define ELF_OSABI_LINUX 3
#define ELF_PT_INTERP 3
#define ELF_PT_PHDR 6
#define ELF_MACHINE_RISCV 243
#define ELF_PT_LOAD 1
#define ELF_PF_X 1
#define ELF_PF_W 2
#define ELF_PF_R 4

typedef struct elf64_header
{
    uint32_t magic;
    uint8_t class;
    uint8_t data;
    uint8_t version;
    uint8_t padding[9];
    uint16_t type;
    uint16_t machine;
    uint32_t file_version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf64_header_t;

typedef struct elf64_program_header
{
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} elf64_program_header_t;

int elf_load(uintptr_t root,
             const uint8_t *image,
             uint64_t size,
             elf_alloc_t alloc,
             void *context,
             uintptr_t *entry)
{
    const elf64_header_t *header = (const elf64_header_t *)image;

    if (size < sizeof(*header) ||
        header->magic != ELF_MAGIC ||
        header->class != ELF_CLASS_64 ||
        header->type != ELF_TYPE_EXEC ||
        header->machine != ELF_MACHINE_RISCV ||
        header->phentsize != sizeof(elf64_program_header_t) ||
        header->phoff + (uint64_t)header->phnum * sizeof(elf64_program_header_t) > size)
    {
        return -1;
    }

    if (header->entry < USER_BASE || header->entry >= USER_CODE_END)
    {
        return -1;
    }

    uintptr_t mapped_end = USER_BASE;

    for (uint16_t i = 0; i < header->phnum; i++)
    {
        const elf64_program_header_t *segment =
            (const elf64_program_header_t *)(image + header->phoff) + i;

        if (segment->type != ELF_PT_LOAD || segment->memsz == 0)
        {
            continue;
        }

        if (segment->filesz > segment->memsz ||
            segment->offset + segment->filesz > size ||
            segment->vaddr < USER_BASE ||
            segment->vaddr + segment->memsz > USER_CODE_END)
        {
            return -1;
        }

        uintptr_t start = segment->vaddr & ~(PAGE_SIZE - 1);
        uintptr_t end = (segment->vaddr + segment->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        /* Segments come in address order and may not share a page: a
           second mapping would silently replace the first one's page */
        if (start < mapped_end)
        {
            return -1;
        }

        mapped_end = end;
        uint8_t *memory = alloc(context, end - start);

        if (memory == 0)
        {
            return -1;
        }

        memcpy(memory + (segment->vaddr - start), image + segment->offset, segment->filesz);

        uint64_t flags = 0;

        flags |= (segment->flags & ELF_PF_R) ? PTE_R : 0;
        flags |= (segment->flags & ELF_PF_W) ? PTE_W : 0;
        flags |= (segment->flags & ELF_PF_X) ? PTE_X : 0;

        if (vm_user_map(root, start, (uintptr_t)memory, end - start, flags) != 0)
        {
            return -1;
        }
    }

    *entry = header->entry;
    return 0;
}

int elf_is_linux(const uint8_t *image, uint64_t size)
{
    const elf64_header_t *header = (const elf64_header_t *)image;

    if (size < sizeof(*header) || header->magic != ELF_MAGIC || header->class != ELF_CLASS_64 ||
        header->machine != ELF_MACHINE_RISCV)
    {
        return 0;
    }

    return header->padding[0] == ELF_OSABI_LINUX || header->type == ELF_TYPE_DYN || header->entry < USER_BASE;
}

int elf_load_linux(const uint8_t *image, uint64_t size, uintptr_t dyn_base, elf_page_t page, void *context,
                   elf_linux_info_t *info)
{
    const elf64_header_t *header = (const elf64_header_t *)image;

    if (!elf_is_linux(image, size) || (header->type != ELF_TYPE_EXEC && header->type != ELF_TYPE_DYN) ||
        header->phentsize != sizeof(elf64_program_header_t) ||
        header->phoff + (uint64_t)header->phnum * sizeof(elf64_program_header_t) > size)
    {
        return -1;
    }

    const elf64_program_header_t *segments = (const elf64_program_header_t *)(image + header->phoff);
    uintptr_t lowest = (uintptr_t)-1;

    info->interp[0] = 0;
    info->phdr = 0;
    info->brk = 0;

    for (uint16_t i = 0; i < header->phnum; i++)
    {
        if (segments[i].type == ELF_PT_INTERP)
        {
            uint64_t n = segments[i].filesz;

            if (segments[i].offset + n > size || n == 0 || n >= ELF_INTERP_MAX)
            {
                return -1;
            }

            memcpy(info->interp, image + segments[i].offset, n);
            info->interp[n - 1] = 0;
        }

        if (segments[i].type == ELF_PT_LOAD && segments[i].memsz && segments[i].vaddr < lowest)
        {
            lowest = segments[i].vaddr;
        }
    }

    if (lowest == (uintptr_t)-1)
    {
        return -1;
    }

    uintptr_t bias = header->type == ELF_TYPE_DYN ? dyn_base - (lowest & ~(PAGE_SIZE - 1)) : 0;

    for (uint16_t i = 0; i < header->phnum; i++)
    {
        const elf64_program_header_t *segment = &segments[i];

        if (segment->type == ELF_PT_PHDR)
        {
            info->phdr = segment->vaddr + bias;
        }

        if (segment->type != ELF_PT_LOAD || segment->memsz == 0)
        {
            continue;
        }

        uintptr_t vaddr = segment->vaddr + bias;

        if (segment->filesz > segment->memsz || segment->offset + segment->filesz > size || vaddr < LINUX_LOW_BASE ||
            vaddr + segment->memsz > LINUX_LOW_END)
        {
            return -1;
        }

        uint64_t rwx = 0;

        rwx |= (segment->flags & ELF_PF_R) ? PTE_R : 0;
        rwx |= (segment->flags & ELF_PF_W) ? PTE_W : 0;
        rwx |= (segment->flags & ELF_PF_X) ? PTE_X : 0;

        uintptr_t start = vaddr & ~(PAGE_SIZE - 1);
        uintptr_t end = (vaddr + segment->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (uintptr_t address = start; address < end; address += PAGE_SIZE)
        {
            uint8_t *memory = page(context, address, rwx);

            if (memory == 0)
            {
                return -1;
            }

            uintptr_t from = address > vaddr ? address : vaddr;
            uintptr_t to = address + PAGE_SIZE < vaddr + segment->filesz ? address + PAGE_SIZE : vaddr + segment->filesz;

            if (to > from)
            {
                memcpy(memory + (from - address), image + segment->offset + (from - vaddr), to - from);
            }
        }

        if (segment->offset == 0 && info->phdr == 0)
        {
            info->phdr = vaddr + header->phoff;
        }

        if (end > info->brk)
        {
            info->brk = end;
        }
    }

    info->entry = header->entry + bias;
    info->base = bias;
    info->phnum = header->phnum;
    info->phent = header->phentsize;
    return 0;
}

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

#include "fdt.h"

/* A flattened device tree (FDT) is how firmware tells the kernel what
   hardware exists. Everything in it is big-endian. The structure block
   is a list of tokens: BEGIN_NODE name, PROP (length, name, value),
   END_NODE, END. We only need /memory (RAM) and /cpus (core count). */

#define FDT_MAGIC 0xD00DFEED
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9
#define FDT_MAX_SIZE (1024 * 1024)

typedef struct fdt_header
{
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
} fdt_header_t;

static uint32_t be32(const void *address)
{
    const uint8_t *b = (const uint8_t *)address;

    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static uint64_t read_cells(const uint8_t *value, uint32_t cells)
{
    uint64_t result = 0;

    for (uint32_t i = 0; i < cells; i++)
    {
        result = (result << 32) | be32(value + i * 4);
    }

    return result;
}

static int starts_with(const char *text, const char *prefix)
{
    while (*prefix)
    {
        if (*text++ != *prefix++)
        {
            return 0;
        }
    }

    return 1;
}

static int text_equal(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }

    return *a == *b;
}

static uint32_t text_length(const char *text)
{
    uint32_t length = 0;

    while (text[length])
    {
        length++;
    }

    return length;
}

int fdt_parse(uintptr_t dtb, fdt_info_t *info)
{
    const uint8_t *base = (const uint8_t *)dtb;
    const fdt_header_t *header = (const fdt_header_t *)dtb;

    info->dtb_start = dtb;
    info->dtb_size = 0;
    info->ram_start = 0;
    info->ram_size = 0;
    info->cpu_count = 0;

    if (dtb == 0 || (dtb & 3) != 0 || be32(&header->magic) != FDT_MAGIC)
    {
        return -1;
    }

    uint32_t total = be32(&header->totalsize);

    if (total > FDT_MAX_SIZE)
    {
        return -1;
    }

    info->dtb_size = total;

    const uint8_t *token = base + be32(&header->off_dt_struct);
    const uint8_t *end = token + be32(&header->size_dt_struct);
    const char *strings = (const char *)base + be32(&header->off_dt_strings);

    uint32_t address_cells = 2;
    uint32_t size_cells = 1;
    int depth = 0;
    int in_memory = 0;
    int in_cpus = 0;

    while (token < end)
    {
        uint32_t type = be32(token);
        token += 4;

        if (type == FDT_BEGIN_NODE)
        {
            const char *name = (const char *)token;

            depth++;

            if (depth == 2)
            {
                in_memory = starts_with(name, "memory");
                in_cpus = text_equal(name, "cpus");
            }
            else if (depth == 3 && in_cpus && starts_with(name, "cpu@"))
            {
                info->cpu_count++;
            }

            token += (text_length(name) + 1 + 3) & ~3U;
        }
        else if (type == FDT_END_NODE)
        {
            if (depth == 2)
            {
                in_memory = 0;
                in_cpus = 0;
            }

            depth--;
        }
        else if (type == FDT_PROP)
        {
            uint32_t length = be32(token);
            const char *name = strings + be32(token + 4);
            const uint8_t *value = token + 8;

            if (depth == 1 && text_equal(name, "#address-cells"))
            {
                address_cells = be32(value);
            }
            else if (depth == 1 && text_equal(name, "#size-cells"))
            {
                size_cells = be32(value);
            }
            else if (depth == 2 && in_memory && info->ram_size == 0 &&
                     text_equal(name, "reg") &&
                     length >= (address_cells + size_cells) * 4)
            {
                info->ram_start = read_cells(value, address_cells);
                info->ram_size = read_cells(value + address_cells * 4, size_cells);
            }

            token += 8 + ((length + 3) & ~3U);
        }
        else if (type == FDT_NOP)
        {
            continue;
        }
        else
        {
            break;
        }
    }

    return info->ram_size != 0 ? 0 : -1;
}

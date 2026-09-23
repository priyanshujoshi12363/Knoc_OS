#include <stdint.h>
#include "string.h"

void *memcpy(void *destination, const void *source, size_t length)
{
    unsigned char *to = (unsigned char *)destination;
    const unsigned char *from = (const unsigned char *)source;

    for (size_t i = 0; i < length; i++)
    {
        to[i] = from[i];
    }

    return destination;
}

void *memset(void *destination, int value, size_t length)
{
    unsigned char *to = (unsigned char *)destination;
    uint64_t pattern = (unsigned char)value * 0x0101010101010101ULL;

    while (length > 0 && ((uintptr_t)to & 7) != 0)
    {
        *to++ = (unsigned char)value;
        length--;
    }

    /* 8 bytes per store: user programs get large blocks cleared */
    uint64_t *words = (uint64_t *)to;

    while (length >= 64)
    {
        words[0] = pattern;
        words[1] = pattern;
        words[2] = pattern;
        words[3] = pattern;
        words[4] = pattern;
        words[5] = pattern;
        words[6] = pattern;
        words[7] = pattern;
        words += 8;
        length -= 64;
    }

    to = (unsigned char *)words;

    while (length > 0)
    {
        *to++ = (unsigned char)value;
        length--;
    }

    return destination;
}

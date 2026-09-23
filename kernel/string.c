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

    for (size_t i = 0; i < length; i++)
    {
        to[i] = (unsigned char)value;
    }

    return destination;
}

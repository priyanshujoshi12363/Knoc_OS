#ifndef _KNOC_ALLOCA_H
#define _KNOC_ALLOCA_H

#include <stddef.h>

#if defined __GNUC__ && !defined __TINYC__
#define alloca(size) __builtin_alloca(size)
#else
void *alloca(size_t size);
#endif

#endif

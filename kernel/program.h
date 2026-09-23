#ifndef PROGRAM_H
#define PROGRAM_H

#include <stdint.h>
#include "process.h"

/* Built-in user programs. Until KnocOS has a filesystem (v0.12), their
   ELF files are embedded in the kernel image (kernel/programs.S). */

typedef struct program
{
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
    process_class_t process_class;
    uint32_t capabilities;
} program_t;

const program_t *program_find(const char *name);
uint32_t program_count(void);

#endif

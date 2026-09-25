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
    uint32_t flags;
} program_t;

/* The program owns the terminal: it gets the keys the console doesn't use */
#define PROGRAM_TERMINAL 0x1

const program_t *program_find(const char *name);
const program_t *program_installed(const char *name);
uint32_t program_count(void);

#endif

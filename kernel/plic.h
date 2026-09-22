#ifndef PLIC_H
#define PLIC_H

#include <stdint.h>

#define PLIC_BASE 0x0C000000UL
#define PLIC_SIZE 0x400000UL

void plic_init(void);
void plic_enable(uint32_t irq);
uint32_t plic_claim(void);
void plic_complete(uint32_t irq);

#endif

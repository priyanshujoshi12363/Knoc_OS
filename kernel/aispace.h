#ifndef AISPACE_H
#define AISPACE_H

#define AISPACE_BASE 0x90000000
#define AISPACE_SIZE 0x10000000
#define AISPACE_HART 1
#define AISPACE_BOOT_TIMEOUT 10000000

#define CLINT_MSIP_HART0 0x02000000

#define AISPACE_PMP_NAPOT_ADDR ((AISPACE_BASE | ((AISPACE_SIZE / 2) - 1)) >> 2)
#define PMP_ALL_NAPOT_ADDR -1
#define PMP_CFG_KERNEL 0x1F18

#ifndef __ASSEMBLER__

#include <stdint.h>

void aispace_main(uintptr_t dtb);
void aispace_park_core0(void);

#endif

#endif

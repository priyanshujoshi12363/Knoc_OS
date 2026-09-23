#ifndef AISPACE_H
#define AISPACE_H

#define AISPACE_BASE 0x87000000
#define AISPACE_SIZE 0x01000000
#define AISPACE_HART 1

#define AISPACE_PMP_NAPOT_ADDR ((AISPACE_BASE | ((AISPACE_SIZE / 2) - 1)) >> 2)
#define PMP_ALL_NAPOT_ADDR -1
#define PMP_CFG_KERNEL 0x1F18

#ifndef __ASSEMBLER__

void aispace_main(void);

#endif

#endif

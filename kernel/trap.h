#ifndef TRAP_H
#define TRAP_H

#define SIP_SSIP 0x2
#define SIE_SSIE 0x2
#define SIP_SEIP 0x200
#define SIE_SEIE 0x200
#define SSTATUS_SIE 0x2

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct trap_frame
{
    uint64_t ra;
    uint64_t gp;
    uint64_t tp;
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    uint64_t s0;
    uint64_t s1;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
    uint64_t a4;
    uint64_t a5;
    uint64_t a6;
    uint64_t a7;
    uint64_t s2;
    uint64_t s3;
    uint64_t s4;
    uint64_t s5;
    uint64_t s6;
    uint64_t s7;
    uint64_t s8;
    uint64_t s9;
    uint64_t s10;
    uint64_t s11;
    uint64_t t3;
    uint64_t t4;
    uint64_t t5;
    uint64_t t6;
    uint64_t sp;
} trap_frame_t;

void supervisor_trap_handler(trap_frame_t *frame);
uint64_t trap_breakpoint_count(void);
void trap_enable_interrupts(void);

#endif

#endif

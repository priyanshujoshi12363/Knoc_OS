#include "../kernel/timer.h"
#include "../kernel/trap.h"

.section .text
.global _start
.extern kernel_main
.extern timer_interrupt
.extern supervisor_trap_handler

_start:
    la sp, stack_top

    li t0, -1
    csrw pmpaddr0, t0

    li t0, 0x1F
    csrw pmpcfg0, t0

    la t0, machine_stack_top
    csrw mscratch, t0

    la t0, machine_trap
    csrw mtvec, t0

    la t0, supervisor_trap
    csrw stvec, t0

    li t0, 0xB1FF
    csrw medeleg, t0

    li t0, SIP_SSIP | SIP_SEIP
    csrw mideleg, t0

    la t0, supervisor_start
    csrw mepc, t0

    li t0, 0x1800
    csrc mstatus, t0

    li t0, 0x0080
    csrs mstatus, t0

    li t0, 0x0800
    csrs mstatus, t0

    li t0, 0x2
    csrs mcounteren, t0

    li t0, CLINT_MTIME
    ld t1, 0(t0)

    li t2, TIMER_INTERVAL
    add t1, t1, t2

    li t0, CLINT_MTIMECMP
    sd t1, 0(t0)

    li t0, 0x0080
    csrs mie, t0

    mret

supervisor_start:
    call kernel_main

loop:
    j loop

machine_trap:
    csrrw sp, mscratch, sp
    addi sp, sp, -256

    sd ra, 0(sp)
    sd gp, 8(sp)
    sd tp, 16(sp)
    sd t0, 24(sp)
    sd t1, 32(sp)
    sd t2, 40(sp)
    sd s0, 48(sp)
    sd s1, 56(sp)
    sd a0, 64(sp)
    sd a1, 72(sp)
    sd a2, 80(sp)
    sd a3, 88(sp)
    sd a4, 96(sp)
    sd a5, 104(sp)
    sd a6, 112(sp)
    sd a7, 120(sp)
    sd s2, 128(sp)
    sd s3, 136(sp)
    sd s4, 144(sp)
    sd s5, 152(sp)
    sd s6, 160(sp)
    sd s7, 168(sp)
    sd s8, 176(sp)
    sd s9, 184(sp)
    sd s10, 192(sp)
    sd s11, 200(sp)
    sd t3, 208(sp)
    sd t4, 216(sp)
    sd t5, 224(sp)
    sd t6, 232(sp)

    csrr t0, mcause

    li t1, 0x8000000000000007
    bne t0, t1, machine_trap_exception

    call timer_interrupt

    ld ra, 0(sp)
    ld gp, 8(sp)
    ld tp, 16(sp)
    ld t0, 24(sp)
    ld t1, 32(sp)
    ld t2, 40(sp)
    ld s0, 48(sp)
    ld s1, 56(sp)
    ld a0, 64(sp)
    ld a1, 72(sp)
    ld a2, 80(sp)
    ld a3, 88(sp)
    ld a4, 96(sp)
    ld a5, 104(sp)
    ld a6, 112(sp)
    ld a7, 120(sp)
    ld s2, 128(sp)
    ld s3, 136(sp)
    ld s4, 144(sp)
    ld s5, 152(sp)
    ld s6, 160(sp)
    ld s7, 168(sp)
    ld s8, 176(sp)
    ld s9, 184(sp)
    ld s10, 192(sp)
    ld s11, 200(sp)
    ld t3, 208(sp)
    ld t4, 216(sp)
    ld t5, 224(sp)
    ld t6, 232(sp)

    addi sp, sp, 256
    csrrw sp, mscratch, sp
    mret

machine_trap_exception:
    la t1, trap_message
    li t2, 0x10000000

trap_print:
    lbu t3, 0(t1)
    beqz t3, trap_cause
    sb t3, 0(t2)
    addi t1, t1, 1
    j trap_print

trap_cause:
    li t4, 15
    li t5, 0xF

trap_hex:
    slli t6, t4, 2
    srl a0, t0, t6
    and a0, a0, t5

    li a1, 10
    bltu a0, a1, trap_digit

    addi a0, a0, 55
    j trap_output

trap_digit:
    addi a0, a0, 48

trap_output:
    sb a0, 0(t2)
    addi t4, t4, -1
    bgez t4, trap_hex

    li a0, 10
    sb a0, 0(t2)

trap_loop:
    j trap_loop

.align 2
supervisor_trap:
    addi sp, sp, -256

    sd ra, 0(sp)
    sd gp, 8(sp)
    sd tp, 16(sp)
    sd t0, 24(sp)
    sd t1, 32(sp)
    sd t2, 40(sp)
    sd s0, 48(sp)
    sd s1, 56(sp)
    sd a0, 64(sp)
    sd a1, 72(sp)
    sd a2, 80(sp)
    sd a3, 88(sp)
    sd a4, 96(sp)
    sd a5, 104(sp)
    sd a6, 112(sp)
    sd a7, 120(sp)
    sd s2, 128(sp)
    sd s3, 136(sp)
    sd s4, 144(sp)
    sd s5, 152(sp)
    sd s6, 160(sp)
    sd s7, 168(sp)
    sd s8, 176(sp)
    sd s9, 184(sp)
    sd s10, 192(sp)
    sd s11, 200(sp)
    sd t3, 208(sp)
    sd t4, 216(sp)
    sd t5, 224(sp)
    sd t6, 232(sp)

    addi t0, sp, 256
    sd t0, 240(sp)

    mv a0, sp
    call supervisor_trap_handler

    ld ra, 0(sp)
    ld gp, 8(sp)
    ld tp, 16(sp)
    ld t0, 24(sp)
    ld t1, 32(sp)
    ld t2, 40(sp)
    ld s0, 48(sp)
    ld s1, 56(sp)
    ld a0, 64(sp)
    ld a1, 72(sp)
    ld a2, 80(sp)
    ld a3, 88(sp)
    ld a4, 96(sp)
    ld a5, 104(sp)
    ld a6, 112(sp)
    ld a7, 120(sp)
    ld s2, 128(sp)
    ld s3, 136(sp)
    ld s4, 144(sp)
    ld s5, 152(sp)
    ld s6, 160(sp)
    ld s7, 168(sp)
    ld s8, 176(sp)
    ld s9, 184(sp)
    ld s10, 192(sp)
    ld s11, 200(sp)
    ld t3, 208(sp)
    ld t4, 216(sp)
    ld t5, 224(sp)
    ld t6, 232(sp)

    addi sp, sp, 256
    sret

trap_message:
    .asciz "[TRAP] mcause=0x"

.section .bss
.align 4
machine_stack:
    .space 4096
machine_stack_top:

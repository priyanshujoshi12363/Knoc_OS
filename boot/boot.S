.section .text
.global _start
.extern kernel_main

_start:
    la sp, stack_top

    li t0, -1
    csrw pmpaddr0, t0

    li t0, 0x1F
    csrw pmpcfg0, t0

    la t0, machine_trap
    csrw mtvec, t0

    la t0, supervisor_start
    csrw mepc, t0

    li t0, 0x1800
    csrc mstatus, t0

    li t0, 0x0800
    csrs mstatus, t0

    mret

supervisor_start:
    call kernel_main

loop:
    j loop

machine_trap:
    csrr t0, mcause
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

trap_message:
    .asciz "[TRAP] mcause=0x"
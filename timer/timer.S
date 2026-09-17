.section .text
.global _start

_start:
    li t2, 0x10000000
    li s0, 0x0200BFF8

loop:
    ld t1, 0(s0)

    la t3, message

print_message:
    lbu t4, 0(t3)
    beqz t4, print_timer
    sb t4, 0(t2)
    addi t3, t3, 1
    j print_message

print_timer:
    mv a0, t1
    li t4, 15

print_hex:
    slli t5, t4, 2
    srl t6, a0, t5
    andi t6, t6, 15

    li t5, 10
    bltu t6, t5, digit

    addi t6, t6, 55
    j output

digit:
    addi t6, t6, 48

output:
    sb t6, 0(t2)
    addi t4, t4, -1
    bgez t4, print_hex

    li t6, 10
    sb t6, 0(t2)

delay:
    li t3, 1000000

delay_loop:
    addi t3, t3, -1
    bnez t3, delay_loop

    j loop

message:
    .asciz "mtime = 0x"
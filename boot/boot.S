.section .text
.global _start
.extern kernel_main

_start:
    la sp, stack_top

    call kernel_main

loop:
    j loop
.section .text.start
.global _start

_start:
    call main
    call exit

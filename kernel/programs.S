.macro PROGRAM name
    .section .rodata
    .balign 8
    .global program_\name\()_start
    .global program_\name\()_end
program_\name\()_start:
    .incbin "user/\name\().elf"
program_\name\()_end:
.endm

PROGRAM hello
PROGRAM badcall
PROGRAM noperm
PROGRAM hog
PROGRAM bigmem
PROGRAM crash
PROGRAM spy

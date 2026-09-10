ENTRY(_start)

SECTIONS
{
    . = 0x80000000;

    kernel_start = .;

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(.bss)
        *(COMMON)
    }

    kernel_end = .;

    . = ALIGN(16);

    stack_bottom = .;
    . += 16K;
    stack_top = .;
}
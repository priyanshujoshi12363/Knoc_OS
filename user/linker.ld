ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x1000000000;

    .text : {
        *(.text.start)
        *(.text .text.*)
    } :text

    .rodata : {
        *(.rodata .rodata.* .srodata .srodata.*)
    } :text

    /* The data segment must start on a new page: sharing a page with the
       code segment would let the loader map one over the other */
    . = ALIGN(4096);

    .data : {
        *(.data .data.* .sdata .sdata.*)
    } :data

    .bss : {
        *(.sbss .sbss.* .bss .bss.* COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.riscv.attributes)
    }
}

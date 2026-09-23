ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    aitext PT_LOAD FLAGS(5);
    aidata PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x80000000;

    kernel_start = .;

    .text : {
        *(EXCLUDE_FILE(*aispace.o) .text)
        *(EXCLUDE_FILE(*aispace.o) .text.*)
    } :text

    .rodata : {
        *(EXCLUDE_FILE(*aispace.o) .rodata)
        *(EXCLUDE_FILE(*aispace.o) .rodata.*)
        *(EXCLUDE_FILE(*aispace.o) .srodata)
        *(EXCLUDE_FILE(*aispace.o) .srodata.*)
    } :text

    kernel_code_end = .;

    .data : {
        *(EXCLUDE_FILE(*aispace.o) .data)
        *(EXCLUDE_FILE(*aispace.o) .data.*)
        *(EXCLUDE_FILE(*aispace.o) .sdata)
        *(EXCLUDE_FILE(*aispace.o) .sdata.*)
    } :data

    . = ALIGN(16);

    /* The AI space keeps a clean copy of kernel_start..kernel_image_end */
    kernel_image_end = .;

    .bss : ALIGN(16) {
        bss_start = .;
        *(EXCLUDE_FILE(*aispace.o) .sbss)
        *(EXCLUDE_FILE(*aispace.o) .sbss.*)
        *(EXCLUDE_FILE(*aispace.o) .bss)
        *(EXCLUDE_FILE(*aispace.o) .bss.*)
        *(EXCLUDE_FILE(*aispace.o) COMMON)
        . = ALIGN(16);
        bss_end = .;
    } :data

    .mailbox : ALIGN(64) {
        *(.mailbox)
    } :data

    kernel_end = .;

    . = ALIGN(16);

    stack_bottom = .;
    . += 16K;
    stack_top = .;

    . = 0x87000000;

    aispace_start = .;

    .aispace.text : {
        *aispace.o(.text .text.*)
        *aispace.o(.rodata .rodata.* .srodata .srodata.*)
    } :aitext

    .aispace.data : ALIGN(4096) {
        *aispace.o(.data .data.* .sdata .sdata.*)
        *aispace.o(.bss .bss.* .sbss .sbss.* COMMON)
    } :aidata

    . = ALIGN(16);

    aispace_stack_bottom = .;
    . += 16K;
    aispace_stack_top = .;

    aispace_end = .;

    . = ALIGN(4096);

    aispace_snapshot = .;
    aispace_snapshot_end = 0x87000000 + 0x01000000;
}

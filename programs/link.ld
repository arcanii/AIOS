SECTIONS {
    . = 0x20500000;
    .text : {
        *(.text._start)
        *(.text.zmain)
        *(.text*)
        *(.rodata*)
    }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM*) }
}

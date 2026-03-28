ENTRY(_start)
SECTIONS {
    . = 0x20500000;
    .text : {
        *(.text._start)
        *(.text)
        *(.rodata*)
        *(.data*)
        *(.bss*)
    }
}

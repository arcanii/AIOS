/* Link the E1 reproducer at 0x80000 -- the address the RPi4 firmware relocates
 * ARM64 images to and branches into. No relocator stub: the image runs in place. */
ENTRY(_start)
SECTIONS
{
    . = 0x80000;
    .text : {
        KEEP(*(.text.boot))     /* image header + _entry must be first */
        *(.text*)
    }
    . = ALIGN(4096);
    .rodata : {
        *(.rodata*)             /* includes the 4KB-aligned l1_table */
    }
    . = ALIGN(16);
    .data : { *(.data*) }
    . = ALIGN(4096);
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    . = ALIGN(4096);
    . = . + 0x10000;            /* 64 KB stack (core 0) */
    _stack_top = .;
    . = ALIGN(4096);
    _sec_stacks_base = .;       /* per-core stacks for cores 1-3 (16KB each; slot 0 unused) */
    . = . + 4 * 0x4000;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

OUTPUT_FORMAT("elf32-bigarm")
OUTPUT_ARCH(arm)
ENTRY(_start)

SECTIONS
{
    . = 0x03000000;
    .text : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
    }
    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) }
}

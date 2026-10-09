ENTRY(_start)
SECTIONS
{
   . = 0x8c020000;
   .text : { *(.text.start) *(.text*) *(.rodata*) *(.data*) }
   .bss : { *(.bss*) *(COMMON) }
   /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}

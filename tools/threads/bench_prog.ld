ENTRY(_start)
SECTIONS { . = 0x8c010000; .text : { *(.text.start) *(.text*) } .rodata : { *(.rodata*) } . = ALIGN(0x1000); .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }

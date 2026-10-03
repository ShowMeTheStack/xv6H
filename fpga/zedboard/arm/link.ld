ENTRY(_start)
SECTIONS
{
	. = 0x00100000;
	.text : { *(.text.start) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
	. = ALIGN(16);
	. += 0x4000;
	_stack_top = .;
}

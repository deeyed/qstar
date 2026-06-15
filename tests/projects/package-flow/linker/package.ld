ENTRY(_start)

SECTIONS
{
	. = 0x80000;
	.text : {
		*(.text.boot)
		*(.text*)
	}
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
}

	.section .text.start
	.global _start
_start:
	cpsid	if
	ldr	sp, =_stack_top
	bl	main
1:	b	1b

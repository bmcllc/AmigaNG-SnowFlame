/* crt0.S - início de um programa C compilado com o GCC para o ColdFire V4e do SF-100 (carregado em 0x1000 pelo IPL). */
	.section .text.start,"ax",@progbits
	.globl	_start
_start:
	move.l	#0x00F00000,%sp		/* IPL_STACK */
	lea	__bss_start,%a0
	lea	_end,%a1
1:	cmp.l	%a1,%a0
	bcc.s	2f
	clr.l	(%a0)+
	bra.s	1b
2:	jsr	__rt_init
	jsr	main
	move.l	%d0,__exit_code
	halt
3:	bra.s	3b

	.section .bss
	.globl	__exit_code
	.align	4
__exit_code:
	.long	0

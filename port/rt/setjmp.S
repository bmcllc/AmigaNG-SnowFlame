/* setjmp.S - setjmp/longjmp do ColdFire. jmp_buf: d2-d7, a2-a7 (sp = valor na entrada, apontando para o endereço de retorno) e o endereço de retorno em [12].
   O slot de retorno da pilha fica "morto" depois que setjmp retorna (as chamadas seguintes o sobrescrevem), por isso o PC é guardado à parte.
   Registradores de ponto flutuante (fp2-fp7) não são salvos. */
	.text
	.globl	setjmp
setjmp:
	move.l	4(%sp),%a0
	move.l	(%sp),%a1
	movem.l	%d2-%d7/%a2-%a7,(%a0)
	move.l	%a1,48(%a0)
	clr.l	%d0
	rts

	.globl	longjmp
longjmp:
	move.l	4(%sp),%a0
	move.l	8(%sp),%d0
	bne.s	1f
	moveq	#1,%d0
1:	movem.l	(%a0),%d2-%d7/%a2-%a7
	move.l	48(%a0),%a1
	addq.l	#4,%sp
	jmp	(%a1)

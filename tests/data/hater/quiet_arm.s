@ Code Hater once said something here, and was wrong every time: gcc's
@ and clang's 32-bit ARM and Thumb.
	.syntax unified
	.thumb
	.text
	.globl	vararg
	.type	vararg, %function
vararg:
	push	{r2, r3}
	push	{r7, lr}
	sub	sp, sp, #32
	add	r7, sp, #0
	str	r0, [r7, #12]
	bl	work
	adds	r7, r7, #32
	mov	sp, r7
	pop	{r7, lr}
	add	sp, sp, #8
	bx	lr

	.globl	wide
	.type	wide, %function
wide:
	push.w	{r4, r5, r6, r7, r8, lr}
	subw	sp, sp, #1076
	mov	r8, r0
	bl	work
	addw	sp, sp, #1076
	pop.w	{r4, r5, r6, r7, r8, pc}

	.arm
	.globl	oldstyle
	.type	oldstyle, %function
oldstyle:
	str	lr, [sp, #-4]!
	bl	work
	ldr	pc, [sp], #4

	.globl	table
	.type	table, %function
table:
	push	{r4, lr}
	cmp	r0, #2
	addls	pc, pc, r0, asl #2
	b	.Ldefault
	b	.Lone
	b	.Ltwo
	b	.Ldefault
.Lone:
	mov	r0, #1
	pop	{r4, pc}
.Ltwo:
	mov	r0, #2
	pop	{r4, pc}
.Ldefault:
	mov	r0, #0
	pop	{r4, pc}

	.globl	trap
	.type	trap, %function
trap:
	push	{r4, lr}
	bl	work
	cmp	r0, #0
	popne	{r4, pc}
	.inst	0xe7f000f0
	.size	trap, .-trap
#ifdef __ELF__
	.section	.note.GNU-stack,"",%progbits
#endif

// Code Hater once said something here, and was wrong every time:
// gcc's AArch64, which writes its numbers without #.
	.text
	.globl	big
	.type	big, %function
big:
	sub	sp, sp, #304
	stp	x29, x30, [sp, 112]
	add	x29, sp, 112
	bl	work
	ldp	x29, x30, [sp, 112]
	add	sp, sp, 304
	ret

	.globl	small
	.type	small, %function
small:
	stp	x29, x30, [sp, -48]!
	mov	x29, sp
	str	x19, [sp, 16]
	mov	x19, x0
	bl	work
	cbz	x19, .L2
	ldr	x19, [sp, 16]
	ldp	x29, x30, [sp], 48
	ret
.L2:
	bl	__stack_chk_fail
	.size	small, .-small
	.section	.note.GNU-stack,"",@progbits

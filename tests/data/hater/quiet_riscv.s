# Not x86, not ARM: RISC-V's rules are not the ones Code Hater knows, so it
# says nothing about them (it once read x19 as AArch64's).
	.text
	.globl	crosscall1
	.type	crosscall1, @function
crosscall1:
	addi	sp, sp, -16
	sd	ra, 8(sp)
	sd	x19, 0(sp)
	mv	x19, a0
	jalr	a1
	ld	x19, 0(sp)
	ld	ra, 8(sp)
	addi	sp, sp, 16
	ret
	.section	.note.GNU-stack,"",@progbits

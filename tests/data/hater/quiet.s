# Code Hater once said something here, and was wrong every time: what
# gcc and clang write, and what people write like them.
	.text
	.globl	framed
	.type	framed, @function
framed:
	pushq	%rbp
	movq	%rsp, %rbp
	subq	$16, %rsp
	movl	%edi, -4(%rbp)
	call	work
	leave
	ret
	.size	framed, .-framed

	.globl	padded
	.type	padded, @function
padded:
	pushq	%rax
	call	work
	addq	$8, %rsp
	ret

	.globl	probed
	.type	probed, @function
probed:
	pushq	%rbx
	leaq	-16384(%rsp), %r11
.Lprobe:
	subq	$4096, %rsp
	orq	$0, (%rsp)
	cmpq	%r11, %rsp
	jne	.Lprobe
	subq	$16, %rsp
	call	work
	addq	$16400, %rsp
	popq	%rbx
	ret

	.globl	guarded
	.type	guarded, @function
guarded:
	subq	$24, %rsp
	movq	%fs:40, %rax
	movq	%rax, 8(%rsp)
	call	work
	movq	8(%rsp), %rax
	subq	%fs:40, %rax
	jne	.Lsmash
	addq	$24, %rsp
	ret
.Lsmash:
	call	__stack_chk_fail@PLT
	.size	guarded, .-guarded

	.globl	never
	.type	never, @function
never:
	call	work
	movl	$1, %edi
	call	die
	.size	never, .-never

	.type	die, @function
die:
	call	abort

	.section	.rodata
.Lname:
	.string	"GNU as, Intel syntax (.intel_syntax): just a string"
	.section	".note.GNU-stack","",@progbits

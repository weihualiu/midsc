	.file	"set_sp.c"
	.text
	.p2align 4,,15
.globl set_sp
	.type	set_sp, @function
set_sp:
.LFB0:
	.cfi_startproc
	leaq	3(%rdi), %rax
	ret
	.cfi_endproc
.LFE0:
	.size	set_sp, .-set_sp
	.p2align 4,,15
.globl restore_sp
	.type	restore_sp, @function
restore_sp:
.LFB1:
	.cfi_startproc
	movzbl	(%rsi,%rcx), %eax
	shrq	$3, %rcx
	movb	%al, (%rdi)
	movq	%rdi, %rax
	movb	$3, (%rdx)
	subq	%rcx, %rax
	ret
	.cfi_endproc
.LFE1:
	.size	restore_sp, .-restore_sp
	.ident	"GCC: (GNU) 4.4.7 20120313 (Red Hat 4.4.7-3)"
	.section	.note.GNU-stack,"",@progbits

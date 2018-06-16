	.file	"set_sp.c"
	.text
	.p2align 4,,15
/*************************************************
 * char * set_sp(char *last_fiber_stack)
 * last_fiber_stack  -> %rdi
 ************************************************/

.globl set_sp
	.type	set_sp, @function
set_sp:
.LFB0:
	.cfi_startproc
//保存返址
	movq	(%rsp),%rax
	movq	%rax,-8(%rdi)
//计算rbp与rsp的距离
	movq	%rbp,%rax
	subq	%rsp,%rax
//设定sp到fiber栈
	movq	%rdi,%rbp
	subq	$8,%rbp
	movq	%rbp,%rsp
//新的rbp，距离rsp与原来相同
	addq	%rax,%rbp
	movq	%rdi, %rax
	ret
	.cfi_endproc
.LFE0:
	.size	set_sp, .-set_sp
	.p2align 4,,15
.globl restore_sp
/***********************************************************************
 * char *restore_sp(char *to,char *from,unsigned long BP,unsigned ling size)
 * to -> %rdi,from -> %rsi, BP->%rdx(不用),size->%rcx
 * to,就是在同一个线程，调度器底部，就是应用的栈顶。
 ************************************************************************/
	.type	restore_sp, @function
restore_sp:
.LFB1:
	.cfi_startproc
//按8字节反向拷贝数据
	subq	$8,%rdi
	subq	$8,%rsi
	shrq	$3,%rcx
	std
	rep	movsq
//%rdi应该是在最后单元之后
	cld
	movq	%rdi,%rax
//返址设定
	movq	(%rsp),%rbx
	movq	%rbx,(%rdi)
//设定rbp
	subq	%rsp,%rbp
	addq	%rdi,%rbp
//rsp设定到新位置
	movq	%rdi,%rsp
//返回栈底
	ret
	.cfi_endproc
.LFE1:
	.size	restore_sp, .-restore_sp
	.ident	"GCC: (GNU) 4.4.7 20120313 (Red Hat 4.4.7-3)"
	.section	.note.GNU-stack,"",@progbits

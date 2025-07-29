/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Low-level exception handling code
 *
 * Copyright (C) 2012 ARM Ltd.
 * Authors:	Catalin Marinas <catalin.marinas@arm.com>
 *		Will Deacon <will.deacon@arm.com>
 */

	.macro hw_xcall_save_base_regs
	stp	x0, x1, [sp, #16 * 0]			// save x0~x29
	stp	x2, x3, [sp, #16 * 1]
	stp	x4, x5, [sp, #16 * 2]
	stp	x6, x7, [sp, #16 * 3]
	stp	x8, x9, [sp, #16 * 4]
	stp	x10, x11, [sp, #16 * 5]
	stp	x12, x13, [sp, #16 * 6]
	stp	x14, x15, [sp, #16 * 7]
	stp	x16, x17, [sp, #16 * 8]
	stp	x18, x19, [sp, #16 * 9]
	stp	x20, x21, [sp, #16 * 10]
	stp	x22, x23, [sp, #16 * 11]
	stp	x24, x25, [sp, #16 * 12]
	stp	x26, x27, [sp, #16 * 13]
	stp	x28, x29, [sp, #16 * 14]

	clear_gp_regs					// clear x0~x29
	mrs	x21, sp_el0
	ldr_this_cpu	tsk, __entry_task, x20
	msr	sp_el0, tsk

#ifdef CONFIG_ARM64_PTR_AUTH
alternative_if ARM64_HAS_ADDRESS_AUTH
	/*
	 * Enable IA for in-kernel PAC if the task had it disabled. Although
	 * this could be implemented with an unconditional MRS which would avoid
	 * a load, this was measured to be slower on Cortex-A75 and Cortex-A76.
	 *
	 * Install the kernel IA key only if IA was enabled in the task. If IA
	 * was disabled on kernel exit then we would have left the kernel IA
	 * installed so there is no need to install it again.
	 */
	ldr	x0, [tsk, THREAD_SCTLR_USER]

	tbz	x0, SCTLR_ELx_ENIA_SHIFT, 1f
	__ptrauth_keys_install_kernel_nosync tsk, x20, x22, x23
	b	2f
1:
	mrs	x0, sctlr_el1
	orr	x0, x0, SCTLR_ELx_ENIA
	msr	sctlr_el1, x0
2:
alternative_else_nop_endif
#endif

alternative_if ARM64_HAS_ADDRESS_AUTH
         isb
alternative_else_nop_endif

	mrs	x22, elr_el1
	mrs	x23, spsr_el1
	stp	lr, x21, [sp, #S_LR]  			// save LR,USER SP

	stp	xzr, xzr, [sp, #S_STACKFRAME]
	add	x29, sp, #S_STACKFRAME			// calc FP

	stp	x22, x23, [sp, #S_PC]			// save ELR,SPSR

	mov	w21, #NO_SYSCALL
	str	w21, [sp, #S_SYSCALLNO]


#ifdef CONFIG_ARM64_PSEUDO_NMI
alternative_if_not ARM64_HAS_GIC_PRIO_MASKING
	b	.Lskip_pmr_save\@
alternative_else_nop_endif

	mrs_s	x20, SYS_ICC_PMR_EL1
	str	x20, [sp, #S_PMR_SAVE]
	mov	x20, #GIC_PRIO_IRQON | GIC_PRIO_PSR_I_SET
	msr_s	SYS_ICC_PMR_EL1, x20

.Lskip_pmr_save\@:
#endif
	.endm /* .macro hw_xcall_save_base_regs */

	.macro hw_xcal_restore_base_regs
#ifdef CONFIG_ARM64_PSEUDO_NMI
alternative_if_not ARM64_HAS_GIC_PRIO_MASKING
	b	.Lskip_pmr_restore\@
alternative_else_nop_endif

	ldr	x20, [sp, #S_PMR_SAVE]
	msr_s	SYS_ICC_PMR_EL1, x20

	/* Ensure priority change is seen by redistributor */
alternative_if_not ARM64_HAS_GIC_PRIO_RELAXED_SYNC
	dsb	sy
alternative_else_nop_endif

.Lskip_pmr_restore\@:
#endif

	ldp	x21, x22, [sp, #S_PC]
	ldr	x23, [sp, #S_SP]
	msr	sp_el0, x23			// restore USER SP

#ifdef CONFIG_ARM64_PTR_AUTH
alternative_if ARM64_HAS_ADDRESS_AUTH
	/*
	 * IA was enabled for in-kernel PAC. Disable it now if needed, or
	 * alternatively install the user's IA. All other per-task keys and
	 * SCTLR bits were updated on task switch.
	 *
	 * No kernel C function calls after this.
	 */
	ldr	x0, [tsk, THREAD_SCTLR_USER]
	tbz	x0, SCTLR_ELx_ENIA_SHIFT, 1f
	__ptrauth_keys_install_user tsk, x0, x1, x2
	b	2f
1:
	mrs	x0, sctlr_el1
	bic	x0, x0, SCTLR_ELx_ENIA
	msr	sctlr_el1, x0
2:
alternative_else_nop_endif
#endif

	msr	elr_el1, x21			// restore ELR
	msr	spsr_el1, x22			// restore SPSR

	ldp	x0, x1, [sp, #16 * 0]		// restore x0~x29
	ldp	x2, x3, [sp, #16 * 1]
	ldp	x4, x5, [sp, #16 * 2]
	ldp	x6, x7, [sp, #16 * 3]
	ldp	x8, x9, [sp, #16 * 4]
	ldp	x10, x11, [sp, #16 * 5]
	ldp	x12, x13, [sp, #16 * 6]
	ldp	x14, x15, [sp, #16 * 7]
	ldp	x16, x17, [sp, #16 * 8]
	ldp	x18, x19, [sp, #16 * 9]
	ldp	x20, x21, [sp, #16 * 10]
	ldp	x22, x23, [sp, #16 * 11]
	ldp	x24, x25, [sp, #16 * 12]
	ldp	x26, x27, [sp, #16 * 13]
	ldp	x28, x29, [sp, #16 * 14]

	ldr	lr, [sp, #S_LR]
	add	sp, sp, #PT_REGS_SIZE		// restore sp
	eret
	sb
	.endm /* .macro hw_xcal_restore_base_regs */

SYM_CODE_START(no_xcall_entry)
	ldp	x20, x21, [sp, #0]
	kernel_entry 0, 64
	mov	x0, sp
	bl	el0t_64_sync_handler
	b	ret_to_user
SYM_CODE_END(no_xcall_entry)

SYM_CODE_START(xcall_entry)
	ldp	x20, x21, [sp, #0]
	hw_xcall_save_base_regs
	mov	x0, sp
	bl	el0t_64_xcall_handler
	hw_xcal_restore_base_regs
SYM_CODE_END(xcall_entry)

SYM_CODE_START_LOCAL(el0t_64_hw_xcall)
	stp	x20, x21, [sp, #0]
	ldr_this_cpu	x21, __cpu_xcall_entry, x20
	mov	x20, __NR_syscalls
	/* x8 >= __NR_syscalls */
	cmp	x8, __NR_syscalls
	csel	x20, x8, x20, lt
	ldr	x21, [x21, x20, lsl #3]
	br	x21
SYM_CODE_END(el0t_64_hw_xcall)

	.macro xcall_ventry
	.align 7
.Lventry_start\@:
	/*
	 * This must be the first instruction of the EL0 vector entries. It is
	 * skipped by the trampoline vectors, to trigger the cleanup.
	 */
	b	.Lskip_tramp_vectors_cleanup\@
	mrs	x30, tpidrro_el0
	msr	tpidrro_el0, xzr
.Lskip_tramp_vectors_cleanup\@:
	sub	sp, sp, #PT_REGS_SIZE
	b	el0t_64_hw_xcall
.org .Lventry_start\@ + 128	// Did we overflow the ventry slot?
	.endm

// SPDX-License-Identifier: GPL-2.0-only
/*
 * xcall related code
 *
 * Copyright (C) 2025 Huawei Ltd.
 */

#include <linux/bitmap.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <asm/xcall.h>

static inline int sw_xcall_init_task(struct task_struct *p, struct task_struct *orig)
{
	p->xinfo = kzalloc(sizeof(struct xcall_info), GFP_KERNEL);
	if (!p->xinfo)
		return -ENOMEM;

	if (orig->xinfo) {
		bitmap_copy(TASK_XINFO(p)->xcall_enable, TASK_XINFO(orig)->xcall_enable,
			    __NR_syscalls);
	}

	return 0;
}

#ifdef CONFIG_ACTLR_XCALL_XINT
static const void *default_syscall_table[__NR_syscalls + 1] = {
	[0 ... __NR_syscalls] = no_xcall_entry,
};

asmlinkage DEFINE_PER_CPU(void *, __cpu_xcall_entry) = default_syscall_table;
static inline int hw_xcall_init_task(struct task_struct *p, struct task_struct *orig)
{
	struct hw_xcall_info *p_xinfo, *orig_xinfo;

	p->xinfo = kzalloc(sizeof(struct hw_xcall_info), GFP_KERNEL);
	if (!p->xinfo)
		return -ENOMEM;

	p_xinfo = TASK_HW_XINFO(p);
	spin_lock_init(&p_xinfo->lock);

	if (!orig->xinfo) {
		memcpy(p->xinfo, default_syscall_table, XCALL_ENTRY_SIZE);
		atomic_set(&p_xinfo->xcall_scno_count, 0);
	} else {
		orig_xinfo = TASK_HW_XINFO(orig);
		spin_lock(&orig_xinfo->lock);
		memcpy(p->xinfo, orig->xinfo, XCALL_ENTRY_SIZE);
		atomic_set(&p_xinfo->xcall_scno_count,
			   atomic_read(&orig_xinfo->xcall_scno_count));
		spin_unlock(&orig_xinfo->lock);
	}

	return 0;
}
#endif

int xcall_init_task(struct task_struct *p, struct task_struct *orig)
{
#ifdef CONFIG_ACTLR_XCALL_XINT
	if (system_uses_xcall_xint())
		return hw_xcall_init_task(p, orig);
#endif
	if (static_branch_unlikely(&xcall_enable))
		return sw_xcall_init_task(p, orig);

	return 0;
}

void xcall_task_free(struct task_struct *p)
{
	if (system_uses_xcall_xint() || static_branch_unlikely(&xcall_enable))
		kfree(p->xinfo);
}

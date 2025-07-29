/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_XCALL_H
#define __ASM_XCALL_H

#include <linux/atomic.h>
#include <linux/jump_label.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/types.h>

#include <asm/actlr.h>
#include <asm/cpufeature.h>

DECLARE_STATIC_KEY_FALSE(xcall_enable);

struct xcall_info {
	/* Must be first! */
	DECLARE_BITMAP(xcall_enable, __NR_syscalls);
};

#define TASK_XINFO(p)	((struct xcall_info *)p->xinfo)

int xcall_init_task(struct task_struct *p, struct task_struct *orig);
void xcall_task_free(struct task_struct *p);

#ifdef CONFIG_ACTLR_XCALL_XINT
struct hw_xcall_info {
	/* Must be first! */
	void *xcall_entry[__NR_syscalls + 1];
	atomic_t xcall_scno_count;
	/* keep xcall_entry and xcall scno count consistent */
	spinlock_t lock;
};

#define TASK_HW_XINFO(p)	((struct hw_xcall_info *)p->xinfo)
#define XCALL_ENTRY_SIZE	(sizeof(unsigned long) * (__NR_syscalls + 1))

DECLARE_PER_CPU(void *, __cpu_xcall_entry);
extern void xcall_entry(void);
extern void no_xcall_entry(void);

static inline bool is_xcall_entry(struct hw_xcall_info *xinfo, unsigned int sc_no)
{
	return xinfo->xcall_entry[sc_no] == xcall_entry;
}

static inline int set_hw_xcall_entry(struct hw_xcall_info *xinfo,
				     unsigned int sc_no, bool enable)
{
	spin_lock(&xinfo->lock);
	if (enable && !is_xcall_entry(xinfo, sc_no)) {
		xinfo->xcall_entry[sc_no] = xcall_entry;
		atomic_inc(&xinfo->xcall_scno_count);
	}

	if (!enable && is_xcall_entry(xinfo, sc_no)) {
		xinfo->xcall_entry[sc_no] = no_xcall_entry;
		atomic_dec(&xinfo->xcall_scno_count);
	}
	spin_unlock(&xinfo->lock);

	return 0;
}

static inline void cpu_set_arch_xcall(bool enable)
{
	u64 el = read_sysreg(CurrentEL);
	u64 val;

	if (el == CurrentEL_EL2) {
		val = read_sysreg(actlr_el2);
		val = enable ? (val | ACTLR_ELx_XCALL) : (val & ~ACTLR_ELx_XCALL);
		write_sysreg(val, actlr_el2);
	} else {
		val = read_sysreg(actlr_el1);
		val = enable ? (val | ACTLR_ELx_XCALL) : (val & ~ACTLR_ELx_XCALL);
		write_sysreg(val, actlr_el1);
	}
}

static inline void cpu_switch_xcall_entry(struct task_struct *tsk)
{
	struct hw_xcall_info *xinfo = tsk->xinfo;

	if (!system_uses_xcall_xint() || !tsk->xinfo)
		return;

	if (unlikely(atomic_read(&xinfo->xcall_scno_count) > 0)) {
		__this_cpu_write(__cpu_xcall_entry, xinfo->xcall_entry);
		cpu_set_arch_xcall(true);
	} else
		cpu_set_arch_xcall(false);
}
#endif /* CONFIG_ACTLR_XCALL_XINT */

#endif /*__ASM_XCALL_H*/

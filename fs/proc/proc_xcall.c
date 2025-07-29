// SPDX-License-Identifier: GPL-2.0
/*
 * xcall related proc code
 *
 * Copyright (C) 2025 Huawei Ltd.
 */
#include <linux/cpufeature.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <asm/xcall.h>
#include "internal.h"

#ifdef CONFIG_ACTLR_XCALL_XINT
static void proc_hw_xcall_show(struct task_struct *p, struct seq_file *m)
{
	struct hw_xcall_info *hw_xinfo = TASK_HW_XINFO(p);
	unsigned int i, start = 0, end = 0;
	bool in_range = false;

	if (!hw_xinfo)
		return;

	for (i = 0; i < __NR_syscalls; i++) {
		bool scno_xcall_enable = is_xcall_entry(hw_xinfo, i);

		if (scno_xcall_enable && !in_range) {
			in_range = true;
			start = i;
		}

		if ((!scno_xcall_enable || i == __NR_syscalls - 1) && in_range) {
			in_range = false;
			end = scno_xcall_enable ? i : i - 1;
			if (i == start + 1)
				seq_printf(m, "%u,", start);
			else
				seq_printf(m, "%u-%u,", start, end);
		}
	}
	seq_puts(m, "\n");
}

static int proc_set_hw_xcall(struct task_struct *p, unsigned int sc_no,
			     bool is_clear)
{
	struct hw_xcall_info *hw_xinfo = TASK_HW_XINFO(p);

	if (!is_clear)
		return set_hw_xcall_entry(hw_xinfo, sc_no, true);

	if (is_clear)
		return set_hw_xcall_entry(hw_xinfo, sc_no, false);

	return -EINVAL;
}
#endif

static int xcall_show(struct seq_file *m, void *v)
{
	struct inode *inode = m->private;
	struct task_struct *p;
	unsigned int rs, re;
	struct xcall_info *xinfo;

	if (!system_uses_xcall_xint() && !static_key_enabled(&xcall_enable))
		return -EACCES;

	p = get_proc_task(inode);
	if (!p)
		return -ESRCH;

#ifdef CONFIG_ACTLR_XCALL_XINT
	if (system_uses_xcall_xint()) {
		proc_hw_xcall_show(p, m);
		goto out;
	}
#endif

	xinfo = TASK_XINFO(p);
	if (!xinfo)
		goto out;

	for (rs = 0, bitmap_next_set_region(xinfo->xcall_enable, &rs, &re, __NR_syscalls);
	     rs < re; rs = re + 1,
	     bitmap_next_set_region(xinfo->xcall_enable, &rs, &re, __NR_syscalls)) {
		if (rs == (re - 1))
			seq_printf(m, "%d,", rs);
		else
			seq_printf(m, "%d-%d,", rs, re - 1);
	}
	seq_puts(m, "\n");
out:
	put_task_struct(p);

	return 0;
}

static int xcall_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, xcall_show, inode);
}

static int xcall_enable_one(struct xcall_info *xinfo, unsigned int sc_no)
{
	test_and_set_bit(sc_no, xinfo->xcall_enable);
	return 0;
}

static int xcall_disable_one(struct xcall_info *xinfo, unsigned int sc_no)
{
	test_and_clear_bit(sc_no, xinfo->xcall_enable);
	return 0;
}

static ssize_t xcall_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *offset)
{
	struct inode *inode = file_inode(file);
	struct task_struct *p;
	char buffer[5];
	const size_t maxlen = sizeof(buffer) - 1;
	unsigned int sc_no = __NR_syscalls;
	int ret = 0;
	int is_clear = 0;
	struct xcall_info *xinfo;

	if (!system_uses_xcall_xint() && !static_key_enabled(&xcall_enable))
		return -EACCES;

	memset(buffer, 0, sizeof(buffer));
	if (!count || copy_from_user(buffer, buf, count > maxlen ? maxlen : count))
		return -EFAULT;

	p = get_proc_task(inode);
	if (!p || !p->xinfo)
		return -ESRCH;

	if (buffer[0] == '!')
		is_clear = 1;

	if (kstrtouint(buffer + is_clear, 10, &sc_no)) {
		ret = -EINVAL;
		goto out;
	}

	if (sc_no >= __NR_syscalls) {
		ret = -EINVAL;
		goto out;
	}

#ifdef CONFIG_ACTLR_XCALL_XINT
	if (system_uses_xcall_xint()) {
		ret = proc_set_hw_xcall(p, sc_no, is_clear);
		goto out;
	}
#endif

	xinfo = TASK_XINFO(p);
	if (!is_clear && !test_bit(sc_no, xinfo->xcall_enable))
		ret = xcall_enable_one(xinfo, sc_no);
	else if (is_clear && test_bit(sc_no, xinfo->xcall_enable))
		ret = xcall_disable_one(xinfo, sc_no);
	else
		ret = -EINVAL;

out:
	put_task_struct(p);

	return ret ? ret : count;
}

const struct file_operations proc_pid_xcall_operations = {
	.open		= xcall_open,
	.read		= seq_read,
	.write		= xcall_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

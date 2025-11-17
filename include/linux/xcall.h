/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Huawei.
 */

#ifndef _LINUX_XCALL_H
#define _LINUX_XCALL_H

#include <linux/module.h>

struct xcall_prog_object {
	unsigned long scno;
	unsigned long func;
};

#define PROG_NAME_LEN	64
#define MAX_NR_SCNO	32

struct xcall_prog {
	char name[PROG_NAME_LEN];
	struct module *owner;
	struct list_head list;
	struct xcall_prog_object objs[MAX_NR_SCNO];
	unsigned int nr_scno;
};

#ifdef CONFIG_DYNAMIC_XCALL
extern int xcall_prog_register(struct xcall_prog *prog);
extern void xcall_prog_unregister(struct xcall_prog *prog);
#else /* !CONFIG_DYNAMIC_XCALL */
static inline int xcall_prog_register(struct xcall_prog *prog)
{
	return -EINVAL;
}
static inline void xcall_prog_unregister(struct xcall_prog *prog) {}
#endif /* CONFIG_DYNAMIC_XCALL */

#endif /* _LINUX_XCALL_H */

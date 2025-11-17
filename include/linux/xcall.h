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
#endif /* _LINUX_XCALL_H */

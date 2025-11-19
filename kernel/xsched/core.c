// SPDX-License-Identifier: GPL-2.0+
/*
 * Core kernel scheduler code for XPU device
 *
 * Copyright (C) 2025-2026 Huawei Technologies Co., Ltd
 *
 * Author: Konstantin Meskhidze <konstantin.meskhidze@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>
#include <linux/xsched.h>

int xsched_schedule(void *input_xcu)
{
	return 0;
}

/* Initializes all xsched XCU objects.
 * Should only be called from xsched_xcu_register function.
 */
int xsched_xcu_init(struct xsched_cu *xcu, struct xcu_group *group, int xcu_id)
{
	int err;

	xcu->id = xcu_id;
	xcu->state = XSCHED_XCU_NONE;
	xcu->group = group;

	mutex_init(&xcu->xcu_lock);

	/* This worker should set XCU to XSCHED_XCU_WAIT_IDLE.
	 * If after initialization XCU still has XSCHED_XCU_NONE
	 * status then we can assume that there was a problem
	 * with XCU kthread job.
	 */
	xcu->worker = kthread_run(xsched_schedule, xcu, "xcu_%u", xcu->id);

	if (IS_ERR(xcu->worker)) {
		err = PTR_ERR(xcu->worker);
		xcu->worker = NULL;
		XSCHED_DEBUG("Fail to run the worker to schedule for xcu[%u].", xcu->id);
		return err;
	}
	return 0;
}

// SPDX-License-Identifier: GPL-2.0+
/*
 * Vstream manage for XPU device
 *
 * Copyright (C) 2025-2026 Huawei Technologies Co., Ltd
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
#include <linux/syscalls.h>
#include <linux/vstream.h>
#include <linux/xsched.h>

#if defined(CONFIG_XCU_VSTREAM)

int vstream_alloc(struct vstream_args *arg)
{
	return 0;
}

int vstream_free(struct vstream_args *arg)
{
	return 0;
}

int vstream_kick(struct vstream_args *arg)
{
	return 0;
}

/*
 * vstream_manage_cmd table
 */
static vstream_manage_t(*vstream_command_table[MAX_COMMAND + 1]) = {
	vstream_alloc, // VSTREAM_ALLOC
	vstream_free, // VSTREAM_FREE
	vstream_kick, // VSTREAM_KICK
	NULL // MAX_COMMAND
};

SYSCALL_DEFINE2(vstream_manage, struct vstream_args __user *, arg, int, cmd)
{
	int res = 0;
	struct vstream_args vstream_arg;

	if (cmd < 0 || cmd >= MAX_COMMAND) {
		XSCHED_ERR("Invalid cmd value: %d, valid range is 0 to %d\n", cmd, MAX_COMMAND - 1);
		return -EINVAL;
	}

	if (copy_from_user(&vstream_arg, arg, sizeof(struct vstream_args))) {
		XSCHED_ERR("copy_from_user failed\n");
		return -EFAULT;
	}

	res = vstream_command_table[cmd](&vstream_arg);
	if (copy_to_user(arg, &vstream_arg, sizeof(struct vstream_args))) {
		XSCHED_ERR("copy_to_user failed\n");
		return -EFAULT;
	}

	XSCHED_DEBUG("vstream_manage: cmd %d\n", cmd);
	return res;
}
#else
SYSCALL_DEFINE2(vstream_manage, struct vstream_args __user *, arg, int, cmd)
{
	return 0;
}
#endif

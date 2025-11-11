/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

#ifndef _UAPI_LINUX_MFS_H
#define _UAPI_LINUX_MFS_H

#include <linux/types.h>
#include <linux/ioctl.h>

enum mfs_opcode {
	MFS_OP_READ = 0,
	MFS_OP_FAULT,
	MFS_OP_FAROUND,
};

enum {
	MFS_MODE_NONE = 0,
	MFS_MODE_LOCAL,
	MFS_MODE_REMOTE,
};

#endif /* _UAPI_LINUX_MFS_H */

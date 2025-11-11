/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2025. Huawei Technologies Co., Ltd */

#ifndef _MFS_INTERNAL_H
#define _MFS_INTERNAL_H

#include <linux/fs.h>
#include <linux/spinlock_types.h>

#define MFS_NAME "mfs"

struct mfs_inode {
	struct inode *lower;
	struct inode *cache;
	struct mutex lock;
	struct inode vfs_inode;
};

struct mfs_dentry_info {
	spinlock_t lock;
	struct path lower;
	struct path cache;
};

#endif

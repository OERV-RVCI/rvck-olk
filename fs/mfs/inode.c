// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2025. Huawei Technologies Co., Ltd */

#include "internal.h"

#include <linux/err.h>
#include <linux/fs_stack.h>
#include <linux/namei.h>

static int mfs_inode_eq(struct inode *inode, void *lower_target)
{
	return mfs_lower_inode(inode) == (struct inode *)lower_target;
}

static int mfs_inode_set(struct inode *inode, void *lower_target)
{
	return 0;
}

struct inode *mfs_iget(struct super_block *sb, struct inode *lower_inode,
			 struct path *cache_path)
{
	struct inode *inode, *cache_inode = d_inode(cache_path->dentry);
	struct mfs_inode *vi;
	int err;

	if (!igrab(lower_inode))
		return ERR_PTR(-ESTALE);
	if (!igrab(cache_inode)) {
		err = -ESTALE;
		goto err_put_lower;
	}
	inode = iget5_locked(sb, lower_inode->i_ino,
			     mfs_inode_eq,
			     mfs_inode_set,
			     lower_inode);
	if (!inode) {
		err = -ENOMEM;
		goto err_put_cache;
	}
	/* found in cache */
	if (!(inode->i_state & I_NEW)) {
		iput(cache_inode);
		iput(lower_inode);
		return inode;
	}
	/* new inode */
	vi = MFS_I(inode);
	inode->i_ino = lower_inode->i_ino;
	vi->lower = lower_inode;
	vi->cache = cache_inode;

	fsstack_copy_attr_all(inode, lower_inode);
	fsstack_copy_inode_size(inode, lower_inode);
	unlock_new_inode(inode);
	return inode;
err_put_cache:
	iput(cache_inode);
err_put_lower:
	iput(lower_inode);
	return ERR_PTR(err);
}

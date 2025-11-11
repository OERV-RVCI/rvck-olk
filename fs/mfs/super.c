// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2025. Huawei Technologies Co., Ltd */

#include "internal.h"

#include <linux/module.h>
#include <linux/fs_context.h>

/*
 * Used for alloc_inode
 */
static struct kmem_cache *mfs_inode_cachep;

/*
 * Used for dentry info
 */
static struct kmem_cache *mfs_dentry_cachep;

static void mfs_init_once(void *obj)
{
	struct mfs_inode *i = obj;

	inode_init_once(&i->vfs_inode);
}

static int mfs_init_fs_context(struct fs_context *fc)
{
	return 0;
}

static void mfs_kill_sb(struct super_block *sb)
{
}

static struct file_system_type mfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= MFS_NAME,
	.init_fs_context = mfs_init_fs_context,
	.kill_sb	= mfs_kill_sb,
	.fs_flags	= 0,
};
MODULE_ALIAS_FS(MFS_NAME);

static int __init init_mfs_fs(void)
{
	int err;

	mfs_inode_cachep =
		kmem_cache_create("mfs_inode",
				  sizeof(struct mfs_inode), 0,
				  SLAB_RECLAIM_ACCOUNT, mfs_init_once);
	if (!mfs_inode_cachep)
		return -ENOMEM;

	mfs_dentry_cachep =
		kmem_cache_create("mfs_dentry",
				  sizeof(struct mfs_dentry_info), 0,
				  SLAB_RECLAIM_ACCOUNT, NULL);
	if (!mfs_dentry_cachep) {
		err = -ENOMEM;
		goto err_dentryp;
	}

	err = register_filesystem(&mfs_fs_type);
	if (err)
		goto err_register;

	pr_info("MFS module loaded\n");
	return 0;
err_register:
	kmem_cache_destroy(mfs_dentry_cachep);
err_dentryp:
	kmem_cache_destroy(mfs_inode_cachep);
	return err;
}

static void __exit exit_mfs_fs(void)
{
	unregister_filesystem(&mfs_fs_type);
	kmem_cache_destroy(mfs_dentry_cachep);
	kmem_cache_destroy(mfs_inode_cachep);
	pr_info("MFS module unload\n");
}

module_init(init_mfs_fs);
module_exit(exit_mfs_fs);

MODULE_AUTHOR("Hongbo Li <lihongbo22@huawei.com>");
MODULE_AUTHOR("Xiaojia Huang <huangxiaojia2@huawei.com>");
MODULE_DESCRIPTION("MFS filesystem for Linux");
MODULE_LICENSE("GPL");

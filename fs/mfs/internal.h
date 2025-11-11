/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2025. Huawei Technologies Co., Ltd */

#ifndef _MFS_INTERNAL_H
#define _MFS_INTERNAL_H

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/container_of.h>
#include <linux/spinlock_types.h>
#include <linux/mfs.h>

#define MFS_NAME "mfs"

struct mfs_sb_info {
	int mode;
	char *mtree;
	char *cachedir;
	struct path lower;
	struct path cache;

	int minor;

	struct super_block *sb;
};

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

#define MFS_SB(sb) ((struct mfs_sb_info *)(sb)->s_fs_info)
#define MFS_I(ptr) container_of(ptr, struct mfs_inode, vfs_inode)
#define MFS_D(dent) ((struct mfs_dentry_info *)(dent)->d_fsdata)

static inline struct inode *mfs_lower_inode(const struct inode *i)
{
	return MFS_I(i)->lower;
}

static inline void pathcpy(struct path *dst, const struct path *src)
{
	dst->dentry = src->dentry;
	dst->mnt = src->mnt;
}

static inline void mfs_install_path(const struct dentry *dent,
					 struct path *lpath,
					 struct path *cpath)
{
	spin_lock(&MFS_D(dent)->lock);
	pathcpy(&MFS_D(dent)->lower, lpath);
	pathcpy(&MFS_D(dent)->cache, cpath);
	spin_unlock(&MFS_D(dent)->lock);
}

static inline void mfs_release_path(const struct dentry *dent)
{
	struct path lpath, cpath;

	if (!dent || !dent->d_fsdata)
		return;
	spin_lock(&MFS_D(dent)->lock);
	pathcpy(&lpath, &MFS_D(dent)->lower);
	pathcpy(&cpath, &MFS_D(dent)->cache);
	MFS_D(dent)->lower.dentry = NULL;
	MFS_D(dent)->lower.mnt = NULL;
	MFS_D(dent)->cache.dentry = NULL;
	MFS_D(dent)->cache.mnt = NULL;
	path_put(&lpath);
	path_put(&cpath);
	spin_unlock(&MFS_D(dent)->lock);
}

struct inode *mfs_iget(struct super_block *sb, struct inode *lower_inode,
			  struct path *cache_path);
int mfs_alloc_dentry_info(struct dentry *dentry);
void mfs_free_dentry_info(struct dentry *dentry);

#endif

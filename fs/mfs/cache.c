// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2025. Huawei Technologies Co., Ltd */

#include "internal.h"

/*
 * Used for cache object
 */
static struct kmem_cache *mfs_cobject_cachep;

static int mfs_setup_object(struct mfs_cache_object *object,
				 struct inode *inode,
				 struct path *cache_path)
{
	struct inode *cache_inode = d_inode(cache_path->dentry);
	struct file *cache_file;
	int flags = O_RDONLY;

	if (need_sync_event(inode->i_sb))
		flags = O_RDWR;
	cache_file = kernel_file_open(cache_path, flags | O_LARGEFILE,
				      cache_inode, current_cred());
	if (IS_ERR(cache_file))
		return PTR_ERR(cache_file);
	/*
	 * object belongs to a mfs inode,
	 * this is a reverse pointer, no refcount needed.
	 */
	object->mfs_inode = inode;
	object->cache_file = cache_file;
	init_rwsem(&object->rwsem);
	object->fd = -1;
	object->anon_file = NULL;
	return 0;
}

void mfs_post_event_read(struct mfs_cache_object *object,
			       loff_t off, uint64_t len,
			       struct mfs_syncer *syncer, int op)
{
}

void mfs_cancel_syncer_events(struct mfs_cache_object *object,
			      struct mfs_syncer *syncer)
{
}

struct mfs_cache_object *mfs_alloc_object(struct inode *inode,
					       struct path *cache_path)
{
	struct mfs_cache_object *object;
	int err;

	object = kmem_cache_alloc(mfs_cobject_cachep, GFP_KERNEL);
	if (!object)
		return ERR_PTR(-ENOMEM);

	err = mfs_setup_object(object, inode, cache_path);
	if (err) {
		kmem_cache_free(mfs_cobject_cachep, object);
		return ERR_PTR(err);
	}

	return object;
}

void mfs_free_object(void *data)
{
	struct mfs_cache_object *object = (struct mfs_cache_object *)data;

	fput(object->cache_file);
	kmem_cache_free(mfs_cobject_cachep, object);
}

int mfs_cache_init(void)
{
	mfs_cobject_cachep =
		kmem_cache_create("mfs_object",
				  sizeof(struct mfs_cache_object), 0,
				  SLAB_RECLAIM_ACCOUNT, NULL);
	if (!mfs_cobject_cachep)
		return -ENOMEM;
	return 0;
}

void mfs_cache_exit(void)
{
	kmem_cache_destroy(mfs_cobject_cachep);
}

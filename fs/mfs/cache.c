// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2025. Huawei Technologies Co., Ltd */

#include "internal.h"

#include <linux/mfs.h>

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
	struct mfs_sb_info *sbi = MFS_SB(object->mfs_inode->i_sb);
	struct mfs_caches *caches = &sbi->caches;
	XA_STATE(xas, &caches->events, 0);
	struct mfs_event *event;
	struct mfs_read *msg;
	int ret;

	/* 1. init event struct */
	event = kzalloc(sizeof(*event) + sizeof(*msg), GFP_KERNEL);
	if (!event) {
		pr_warn("post read event failed, off:%lld, len:%llu\n", off, len);
		return;
	}

	/* 2. hold object's owner mfs_inode */
	ihold(object->mfs_inode);
	refcount_set(&event->ref, 1);
	event->object = object;
	event->msg.version = 0;
	event->msg.opcode = op;
	event->msg.len = sizeof(struct mfs_msg) + sizeof(struct mfs_read);
	event->msg.fd = object->fd;
	msg = (void *)event->msg.data;
	msg->off = off;
	msg->len = len;
	msg->pid = current->pid;
	INIT_LIST_HEAD(&event->link);
	event->syncer = syncer;
	if (event->syncer) {
		atomic_inc(&syncer->notback);
		spin_lock(&syncer->list_lock);
		list_add_tail(&event->link, &syncer->head);
		spin_unlock(&syncer->list_lock);
	}

	/* 3. put event into reqs' xarray */
	do {
		xas_lock(&xas);

		if (!test_bit(MFS_CACHE_READY, &caches->flags)) {
			xas_unlock(&xas);
			goto out;
		}

		/* Ensure cache enabled judgement before posting events */
		smp_mb__after_atomic();

		xas.xa_index = caches->next_msg;
		xas_find_marked(&xas, UINT_MAX, XA_FREE_MARK);
		if (xas.xa_node == XAS_RESTART) {
			xas.xa_index = 0;
			xas_find_marked(&xas, caches->next_msg - 1, XA_FREE_MARK);
		}
		if (xas.xa_node == XAS_RESTART)
			xas_set_err(&xas, -EBUSY);
		xas_store(&xas, event);
		if (xas_valid(&xas)) {
			caches->next_msg = xas.xa_index + 1;
			event->msg.id = xas.xa_index;
			xas_clear_mark(&xas, XA_FREE_MARK);
			xas_set_mark(&xas, MFS_EVENT_NEW);
		}
		xas_unlock(&xas);
	} while (xas_nomem(&xas, GFP_KERNEL));

	ret = xas_error(&xas);
	if (ret) {
		pr_warn("post read event failed to insert events, off:%lld, len:%llu, ret:%d\n",
			off, len, ret);
		goto out;
	}

	/* 3. wakeup the polling wait list */
	wake_up_all(&caches->pollwq);
	return;
out:
	if (event->syncer) {
		spin_lock(&syncer->list_lock);
		list_del_init(&event->link);
		spin_unlock(&syncer->list_lock);
		atomic_dec(&syncer->notback);
	}
	kfree(event);
	iput(object->mfs_inode);
}

void mfs_destroy_events(struct super_block *sb)
{
	struct mfs_sb_info *sbi = MFS_SB(sb);
	struct mfs_caches *caches = &sbi->caches;
	unsigned long index;
	struct mfs_event *event;

	xa_lock(&caches->events);
	xa_for_each(&caches->events, index, event) {
		/*
		 * Inodes will be evicted before destroy events.
		 * Hence there should be none of events.
		 */
		pr_warn("Event remains:%lu\n", index);
		__xa_erase(&caches->events, index);
		xa_unlock(&caches->events);
		put_mfs_event(event);
		xa_lock(&caches->events);
	}
	xa_unlock(&caches->events);
	xa_destroy(&caches->events);
}

void mfs_cancel_syncer_events(struct mfs_cache_object *object,
			      struct mfs_syncer *syncer)
{
	struct mfs_sb_info *sbi = MFS_SB(object->mfs_inode->i_sb);
	struct mfs_caches *caches = &sbi->caches;
	struct mfs_event *event;
	struct list_head tmp;

	INIT_LIST_HEAD(&tmp);
	spin_lock(&syncer->list_lock);
	list_splice_init(&syncer->head, &tmp);
	spin_unlock(&syncer->list_lock);

	list_for_each_entry(event, &tmp, link) {
		xa_erase(&caches->events, event->msg.id);
		iput(event->object->mfs_inode);
		kfree(event);
	}
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

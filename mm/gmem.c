// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generalized Memory Management.
 *
 * Copyright (C) 2023- Huawei, Inc.
 * Author: Weixi Zhu
 *
 */

#include <linux/gmem.h>
#include <linux/mm.h>
#include <linux/vm_object.h>
#include <linux/xarray.h>

#include "gmem-internal.h"

DEFINE_STATIC_KEY_FALSE(gmem_status);
EXPORT_SYMBOL_GPL(gmem_status);

static struct kmem_cache *gm_as_cache;
static struct kmem_cache *gm_dev_cache;
static struct kmem_cache *gm_ctx_cache;
static DEFINE_XARRAY_ALLOC(gm_dev_id_pool);

static bool enable_gmem __ro_after_init;

static int __init gmem_init(void)
{
	int err = -ENOMEM;

	if (!enable_gmem)
		return 0;

	gm_as_cache = KMEM_CACHE(gm_as, 0);
	if (!gm_as_cache)
		goto out;

	gm_dev_cache = KMEM_CACHE(gm_dev, 0);
	if (!gm_dev_cache)
		goto free_as;

	gm_ctx_cache = KMEM_CACHE(gm_context, 0);
	if (!gm_ctx_cache)
		goto free_dev;

	err = gm_page_cachep_init();
	if (err)
		goto free_ctx;

	err = vm_object_init();
	if (err)
		goto free_gm_page;

	static_branch_enable(&gmem_status);

	return 0;

free_gm_page:
	gm_page_cachep_destroy();
free_ctx:
	kmem_cache_destroy(gm_ctx_cache);
free_dev:
	kmem_cache_destroy(gm_dev_cache);
free_as:
	kmem_cache_destroy(gm_as_cache);
out:
	return -ENOMEM;
}
subsys_initcall(gmem_init);

static int __init setup_gmem(char *str)
{
	strtobool(str, &enable_gmem);

	return 1;
}
__setup("gmem=", setup_gmem);

/*
 * Create a GMEM device, register its MMU function and the page table.
 * The returned device pointer will be passed by new_dev.
 * A unique id will be assigned to the GMEM device, using Linux's xarray.
 */
int gm_dev_create(struct gm_mmu *mmu, void *dev_data,
				struct gm_dev **new_dev)
{
	struct gm_dev *dev;

	if (!gmem_is_enabled())
		return -EINVAL;

	dev = kmem_cache_alloc(gm_dev_cache, GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	if (xa_alloc(&gm_dev_id_pool, &dev->id, dev, xa_limit_32b,
			 GFP_KERNEL)) {
		kmem_cache_free(gm_dev_cache, dev);
		return -EAGAIN;
	}

	dev->mmu = mmu;
	dev->dev_data = dev_data;
	dev->current_ctx = NULL;
	INIT_LIST_HEAD(&dev->gm_ctx_list);
	*new_dev = dev;
	nodes_clear(dev->registered_hnodes);
	return 0;
}
EXPORT_SYMBOL_GPL(gm_dev_create);


/* GMEM Virtual Address Space API */
int gm_as_create(unsigned long begin, unsigned long end, enum gm_as_alloc policy,
			unsigned long cache_quantum, struct gm_as **new_as)
{
	struct gm_as *as;

	if (!new_as)
		return -EINVAL;

	as = kmem_cache_alloc(gm_as_cache, GFP_ATOMIC);
	if (!as)
		return -ENOMEM;

	spin_lock_init(&as->rbtree_lock);
	as->rbroot = RB_ROOT;
	as->start_va = begin;
	as->end_va = end;
	as->policy = policy;

	INIT_LIST_HEAD(&as->gm_ctx_list);

	*new_as = as;
	return 0;
}
EXPORT_SYMBOL_GPL(gm_as_create);

int gm_as_destroy(struct gm_as *as)
{
	struct gm_context *ctx, *tmp_ctx;

	list_for_each_entry_safe(ctx, tmp_ctx, &as->gm_ctx_list, gm_as_link)
		kfree(ctx);

	kmem_cache_free(gm_as_cache, as);

	return 0;
}
EXPORT_SYMBOL_GPL(gm_as_destroy);

int gm_as_attach(struct gm_as *as, struct gm_dev *dev,
			bool activate, struct gm_context **out_ctx)
{
	struct gm_context *ctx;
	int nid;

	ctx = kmem_cache_alloc(gm_ctx_cache, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->as = as;
	ctx->dev = dev;

	INIT_LIST_HEAD(&ctx->gm_dev_link);
	INIT_LIST_HEAD(&ctx->gm_as_link);

	if (!list_empty(&as->gm_ctx_list)) {
		struct list_head *old_node;
		struct gm_context *old_ctx;

		old_node = as->gm_ctx_list.prev;
		list_del_init(old_node);
		old_ctx = list_entry(old_node, struct gm_context, gm_as_link);
		kfree(old_ctx);
	}

	list_add_tail(&dev->gm_ctx_list, &ctx->gm_dev_link);
	list_add_tail(&ctx->gm_as_link, &as->gm_ctx_list);

	if (activate) {
		/*
		 * Here we should really have a callback function to perform the context switch
		 * for the hardware. E.g. in x86 this function is effectively
		 * flushing the CR3 value. Currently we do not care time-sliced context switch,
		 * unless someone wants to support it.
		 */
		dev->current_ctx = ctx;
	}
	*out_ctx = ctx;

	/*
	 * gm_as_attach will be used to attach device to process address space.
	 * Handle this case and add hnodes registered by device to process mems_allowed.
	 */
	for_each_node_mask(nid, dev->registered_hnodes)
		node_set(nid, current->mems_allowed);
	return 0;
}
EXPORT_SYMBOL_GPL(gm_as_attach);

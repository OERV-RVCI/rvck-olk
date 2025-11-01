// SPDX-License-Identifier: GPL-2.0-only
/*
 * GMEM physical memory management.
 *
 * Copyright (C) 2025- Huawei, Inc.
 * Author: Bin Wang
 *
 */

#include <linux/slab.h>

#include "gmem-internal.h"

static struct kmem_cache *gm_page_cachep;

int __init gm_page_cachep_init(void)
{
	gm_page_cachep = KMEM_CACHE(gm_page, 0);
	if (!gm_page_cachep)
		return -EINVAL;
	return 0;
}

void gm_page_cachep_destroy(void)
{
	kmem_cache_destroy(gm_page_cachep);
}

struct gm_page *alloc_gm_page_struct(void)
{
	struct gm_page *gm_page = kmem_cache_zalloc(gm_page_cachep, GFP_KERNEL);

	if (!gm_page)
		return NULL;
	atomic_set(&gm_page->refcount, 0);
	spin_lock_init(&gm_page->rmap_lock);
	return gm_page;
}
EXPORT_SYMBOL(alloc_gm_page_struct);

void gm_free_page(struct gm_page *gm_page)
{
    // TODO: add gm_page to freelist
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * GMEM physical memory management.
 *
 * Copyright (C) 2025- Huawei, Inc.
 * Author: Bin Wang
 *
 */

#include <linux/pgtable.h>
#include <linux/slab.h>

#include "gmem-internal.h"

static struct kmem_cache *gm_page_cachep;


DEFINE_SPINLOCK(hnode_lock);
static nodemask_t hnode_map;
struct hnode *hnodes[MAX_NUMNODES];

void __init hnuma_init(void)
{
	unsigned int node;

	spin_lock(&hnode_lock);
	nodes_clear(hnode_map);
	for_each_node(node)
		node_set(node, hnode_map);
	spin_unlock(&hnode_lock);
}

bool is_hnode(int nid)
{
	return (nid < MAX_NUMNODES) && !node_isset(nid, node_possible_map) &&
			node_isset(nid, hnode_map);
}

unsigned int alloc_hnode_id(void)
{
	unsigned int node;

	node = first_unset_node(hnode_map);
	node_set(node, hnode_map);

	return node;
}

void free_hnode_id(unsigned int nid)
{
	node_clear(nid, hnode_map);
}

void hnode_init(struct hnode *hnode, unsigned int hnid, struct gm_dev *dev)
{
	hnode->id = hnid;
	hnode->dev = dev;
	INIT_LIST_HEAD(&hnode->freelist);
	INIT_LIST_HEAD(&hnode->activelist);
	spin_lock_init(&hnode->freelist_lock);
	spin_lock_init(&hnode->activelist_lock);
	atomic_set(&hnode->nr_free_pages, 0);
	atomic_set(&hnode->nr_active_pages, 0);
	hnode->import_failed = false;
	hnode->max_memsize = 0;

	node_set(hnid, dev->registered_hnodes);
	hnodes[hnid] = hnode;
}

void hnode_deinit(unsigned int hnid, struct gm_dev *dev)
{
	hnodes[hnid]->id = 0;
	hnodes[hnid]->dev = NULL;
	node_clear(hnid, dev->registered_hnodes);
	hnodes[hnid] = NULL;
}

struct hnode *get_hnode(unsigned int hnid)
{
	if (!hnodes[hnid])
		gmem_err("h-NUMA node for hnode id %u is NULL.", hnid);
	return hnodes[hnid];
}

struct gm_dev *get_gm_dev(unsigned int nid)
{
	struct hnode *hnode;
	struct gm_dev *dev = NULL;

	spin_lock(&hnode_lock);
	hnode = get_hnode(nid);
	if (hnode)
		dev = hnode->dev;
	spin_unlock(&hnode_lock);
	return dev;
}

int gm_dev_register_hnode(struct gm_dev *dev)
{
	unsigned int hnid;
	struct hnode *hnode = kmalloc(sizeof(struct hnode), GFP_KERNEL);
	int ret;

	if (!hnode)
		return -ENOMEM;

	spin_lock(&hnode_lock);
	hnid = alloc_hnode_id();
	spin_unlock(&hnode_lock);

	if (hnid == MAX_NUMNODES)
		goto free_hnode;

	ret = hnode_init_sysfs(hnid);
	if (ret)
		goto free_hnode;

	hnode_init(hnode, hnid, dev);

	return GM_RET_SUCCESS;

free_hnode:
	kfree(hnode);
	return -EBUSY;
}
EXPORT_SYMBOL_GPL(gm_dev_register_hnode);

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

void hnode_freelist_add(struct hnode *hnode, struct gm_page *gm_page)
{
	spin_lock(&hnode->freelist_lock);
	list_add(&gm_page->gm_page_list, &hnode->freelist);
	spin_unlock(&hnode->freelist_lock);
}

void hnode_activelist_add(struct hnode *hnode, struct gm_page *gm_page)
{
	spin_lock(&hnode->activelist_lock);
	list_add_tail(&gm_page->gm_page_list, &hnode->activelist);
	spin_unlock(&hnode->activelist_lock);
}

void hnode_activelist_del(struct hnode *hnode, struct gm_page *gm_page)
{
	spin_lock(&hnode->activelist_lock);
	/* If a gm_page is being evicted, it is currently located in the
	 * temporary linked list. */
	list_del_init(&gm_page->gm_page_list);
	spin_unlock(&hnode->activelist_lock);
}

void hnode_activelist_del_and_add(struct hnode *hnode, struct gm_page *gm_page)
{
	spin_lock(&hnode->activelist_lock);
	list_move_tail(&gm_page->gm_page_list, &hnode->activelist);
	spin_unlock(&hnode->activelist_lock);
}

int gm_add_pages(unsigned int hnid, struct list_head *pages)
{
	struct hnode *hnode;
	struct gm_page *gm_page, *n;

	hnode = get_hnode(hnid);
	if (!hnode)
		return -EINVAL;

	list_for_each_entry_safe(gm_page, n, pages, gm_page_list) {
		list_del(&gm_page->gm_page_list);
		hnode_freelist_add(hnode, gm_page);
		hnode_free_pages_inc(hnode);
	}

	return 0;
}
EXPORT_SYMBOL(gm_add_pages);

void gm_free_page(struct gm_page *gm_page)
{
	struct hnode *hnode;

	hnode = get_hnode(gm_page->hnid);
	if (!hnode)
		return;
	hnode_freelist_add(hnode, gm_page);
	hnode_free_pages_inc(hnode);
}

void gm_page_add_rmap(struct gm_page *gm_page, struct mm_struct *mm, unsigned long va)
{
	spin_lock(&gm_page->rmap_lock);
	gm_page->mm = mm;
	gm_page->va = va;
	spin_unlock(&gm_page->rmap_lock);
}

void gm_page_remove_rmap(struct gm_page *gm_page)
{
	spin_lock(&gm_page->rmap_lock);
	gm_page->mm = NULL;
	gm_page->va = 0;
	spin_unlock(&gm_page->rmap_lock);
}

static bool can_import(struct hnode *hnode)
{
	unsigned long nr_pages;
	unsigned long used_mem;

	nr_pages = atomic_read(&hnode->nr_free_pages) + atomic_read(&hnode->nr_active_pages);
	used_mem = nr_pages * HPAGE_SIZE;

	/* GMEM usable memory is unlimited if max_memsize is zero. */
	if (!hnode->max_memsize)
		return true;
	return used_mem < hnode->max_memsize;
}

static struct gm_page *get_gm_page_from_freelist(struct hnode *hnode)
{
	struct gm_page *gm_page;

	spin_lock(&hnode->freelist_lock);
	gm_page = list_first_entry_or_null(&hnode->freelist, struct gm_page, gm_page_list);
	/* Delete from freelist. */
	if (gm_page) {
		list_del_init(&gm_page->gm_page_list);
		hnode_free_pages_dec(hnode);
		get_gm_page(gm_page);
	}
	spin_unlock(&hnode->freelist_lock);

	return gm_page;
}

/*
 * gm_alloc_page - Allocate a gm_page.
 *
 * Allocate a gm_page from hnode freelist. If failed to allocate gm_page, try
 * to import memory from device. And if failed to import memory, try to swap
 * several gm_pages to host and allocate gm_page again.
 */
struct gm_page *gm_alloc_page(struct mm_struct *mm, struct hnode *hnode)
{
	struct gm_page *gm_page;
	struct gm_dev *gm_dev;
	int ret = 0;

	if (hnode->dev)
		gm_dev = hnode->dev;
	else
		return NULL;

retry:
	gm_page = get_gm_page_from_freelist(hnode);
	if (!gm_page && can_import(hnode) && !hnode->import_failed) {
		/* Import pages from device. */
		ret = gm_dev->mmu->import_phys_mem(mm, hnode->id, NUM_IMPORT_PAGES);
		if (!ret)
			goto retry;
		hnode->import_failed = true;
	}

	return gm_page;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generalized Memory Management.
 *
 * Copyright (C) 2023- Huawei, Inc.
 * Author: Weixi Zhu
 *
 */

#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
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

static inline unsigned long pe_mask(unsigned int order)
{
	if (order == 0)
		return PAGE_MASK;
	if (order == PMD_ORDER)
		return HPAGE_PMD_MASK;
	if (order == PUD_ORDER)
		return HPAGE_PUD_MASK;
	return 0;
}

static int __init gmem_init(void)
{
	int err = -ENOMEM;

	if (!enable_gmem)
		return 0;

	hnuma_init();

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

/* Handle the page fault triggered by a given device with mmap lock*/
enum gm_ret gm_dev_fault_locked(struct mm_struct *mm, unsigned long  addr, struct gm_dev *dev,
				int behavior)
{
	enum gm_ret ret = GM_RET_SUCCESS;
	struct gm_mmu *mmu = dev->mmu;
	struct hnode *hnode;
	struct device *dma_dev = dev->dma_dev;
	struct vm_area_struct *vma;
	struct gm_mapping *gm_mapping;
	struct gm_page *gm_page;
	unsigned long size = HPAGE_SIZE;
	struct gm_fault_t gmf = {
		.mm = mm,
		.va = addr,
		.dev = dev,
		.size = size,
		.copy = false,
		.behavior = behavior
	};
	struct page *page = NULL;

	hnode = get_hnode(get_hnuma_id(dev));
	if (!hnode) {
		gmem_err("gmem device should correspond to a hnuma node");
		ret = -EINVAL;
		goto out;
	}

	vma = find_vma(mm, addr);
	if (!vma || vma->vm_start > addr) {
		gmem_err("%s failed to find vma", __func__);
		ret = GM_RET_FAILURE_UNKNOWN;
		goto out;
	}

	gm_mapping = vma_prepare_gm_mapping(vma, addr);
	if (unlikely(!gm_mapping)) {
		if (!vma->vm_obj) {
			gmem_err("%s no vm_obj", __func__);
			ret = GM_RET_FAILURE_UNKNOWN;
		} else {
			gmem_err("%s OOM when creating vm_obj!", __func__);
			ret = GM_RET_NOMEM;
		}
		goto out;
	}

	mutex_lock(&gm_mapping->lock);
	if (gm_mapping_nomap(gm_mapping)) {
		goto peer_map;
	} else if (gm_mapping_device(gm_mapping)) {
		goto unlock;
	} else if (gm_mapping_cpu(gm_mapping)) {
		page = gm_mapping->page;
		if (!page) {
			gmem_err("host gm_mapping page is NULL. Set nomap");
			gm_mapping_flags_set(gm_mapping, GM_MAPPING_NOMAP);
			goto unlock;
		}
		get_page(page);
		/* zap_page_range_single can be used in Linux 6.4 and later versions. */
		zap_page_range_single(vma, addr, size, NULL);
		gmf.dma_addr =
			dma_map_page(dma_dev, page, 0, size, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dma_dev, gmf.dma_addr))
			gmem_err("dma map failed");

		gmf.copy = true;
	}

peer_map:
	gm_page = gm_alloc_page(mm, hnode);
	if (!gm_page) {
		gmem_err("Alloc gm_page for device fault failed.");
		ret = -ENOMEM;
		goto unlock;
	}

	gmf.pfn = gm_page->dev_pfn;

	ret = mmu->peer_map(&gmf);
	if (ret != GM_RET_SUCCESS) {
		gmem_err("peer map failed");
		if (page)
			gm_mapping_flags_set(gm_mapping, GM_MAPPING_CPU);
		put_gm_page(gm_page);
		goto unlock;
	}

	if (page) {
		dma_unmap_page(dma_dev, gmf.dma_addr, size, DMA_BIDIRECTIONAL);
		folio_put(page_folio(page));
	}

	gm_mapping_flags_set(gm_mapping, GM_MAPPING_DEVICE);
	gm_mapping->dev = dev;
	gm_page_add_rmap(gm_page, mm, addr);
	gm_mapping->gm_page = gm_page;

	hnode_activelist_add(hnode, gm_page);
	hnode_active_pages_inc(hnode);
unlock:
	mutex_unlock(&gm_mapping->lock);
out:
	return ret;
}
EXPORT_SYMBOL_GPL(gm_dev_fault_locked);

vm_fault_t gm_host_fault_locked(struct vm_fault *vmf,
				unsigned int order)
{
	vm_fault_t ret = 0;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long addr = vmf->address & pe_mask(order);
	struct vm_object *obj = vma->vm_obj;
	struct gm_mapping *gm_mapping;
	unsigned long size = HPAGE_SIZE;
	struct gm_dev *dev;
	struct hnode *hnode;
	struct device *dma_dev;
	struct gm_fault_t gmf = {
		.mm = vma->vm_mm,
		.va = addr,
		.size = size,
		.copy = true,
	};

	gm_mapping = vm_object_lookup(obj, addr);
	if (!gm_mapping) {
		gmem_err("host fault gm_mapping should not be NULL\n");
		return VM_FAULT_SIGBUS;
	}

	dev = gm_mapping->dev;
	gmf.dev = dev;
	gmf.pfn = gm_mapping->gm_page->dev_pfn;
	dma_dev = dev->dma_dev;
	gmf.dma_addr =
		dma_map_page(dma_dev, vmf->page, 0, size, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dma_dev, gmf.dma_addr)) {
		gmem_err("host fault dma mapping error\n");
		return VM_FAULT_SIGBUS;
	}
	if (dev->mmu->peer_unmap(&gmf) != GM_RET_SUCCESS) {
		gmem_err("peer unmap failed\n");
		dma_unmap_page(dma_dev, gmf.dma_addr, size, DMA_BIDIRECTIONAL);
		return VM_FAULT_SIGBUS;
	}

	dma_unmap_page(dma_dev, gmf.dma_addr, size, DMA_BIDIRECTIONAL);
	hnode = get_hnode(gm_mapping->gm_page->hnid);
	gm_page_remove_rmap(gm_mapping->gm_page);
	hnode_activelist_del(hnode, gm_mapping->gm_page);
	hnode_active_pages_dec(hnode);
	put_gm_page(gm_mapping->gm_page);
	return ret;
}

enum gm_ret gm_dev_fault(struct mm_struct *mm, unsigned long  addr,
				struct gm_dev *dev, int behavior)
{
	int ret;

	mmap_read_lock(mm);
	ret = gm_dev_fault_locked(mm, addr, dev, behavior);
	mmap_read_unlock(mm);

	return ret;
}
EXPORT_SYMBOL_GPL(gm_dev_fault);

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

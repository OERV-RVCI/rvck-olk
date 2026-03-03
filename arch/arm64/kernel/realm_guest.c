// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025. Huawei Technologies Co., Ltd. All rights reserved.
 */
#include <linux/io.h>
#include <linux/swiotlb.h>
#include <asm/rsi.h>
#include <asm/realm_guest.h>

static struct device realm_alloc_device;

/*
 * struct io_tlb_no_swiotlb_mem - whether use the
 * bounce buffer mechanism or not
 * @for_alloc: %true if the pool is used for memory allocation.
 *	Here it is set to %false, to force devices to use direct dma operations.
 *
 * @force_bounce: %true if swiotlb bouncing is forced.
 *	Here it is set to %false, to force devices to use direct dma operations.
 */
static struct io_tlb_mem io_tlb_no_swiotlb_mem = {
	.for_alloc = false,
	.force_bounce = false,
};

void enable_swiotlb_for_realm_dev(struct device *dev, bool enable)
{
	if (!is_realm_world())
		return;

	if (enable)
		swiotlb_dev_init(dev);
	else
		dev->dma_io_tlb_mem = &io_tlb_no_swiotlb_mem;
}
EXPORT_SYMBOL_GPL(enable_swiotlb_for_realm_dev);

void __init realm_guest_init(void)
{
	device_initialize(&realm_alloc_device);
	enable_swiotlb_for_realm_dev(&realm_alloc_device, true);
}

struct page *realm_alloc_swiotlb_shared_pages(gfp_t gfp, unsigned int order)
{
	return swiotlb_alloc(&realm_alloc_device, (1UL << order) * PAGE_SIZE);
}

bool realm_free_swiotlb_shared_pages(void *addr, unsigned int order)
{
	return swiotlb_free(&realm_alloc_device, (struct page *)addr,
			    (1UL << order) * PAGE_SIZE);
}

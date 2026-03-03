// SPDX-License-Identifier: GPL-2.0-only
/*
 * CCA Realm page table allocator.
 *
 * Copyright (c) 2026 Hisilicon Limited.
 * Author: Zhang Wei <zhangwei375@huawei.com>
 */

#define pr_fmt(fmt)	"realm io-pgtable: " fmt

#include <linux/pci.h>
#include <asm/rmi_cmds.h>
#include <linux/io-pgtable.h>

#include "io-pgtable-arm.h"

#define REALM_MAX_ADDR_BITS		48
#define REALM_S2_MAX_CONCAT_PAGES	16
#define REALM_MAX_LEVELS		4

/* Register bits */
#define REALM_VTCR_SL0_MASK		0x3

/* Struct accessors */
#define io_pgtable_to_data(x)						\
	container_of((x), struct realm_io_pgtable, iop)

#define REALM_PGD_SIZE(d)						\
	(sizeof(realm_iopte) << (d)->pgd_bits)

typedef u64 realm_iopte;

struct realm_io_pgtable {
	struct io_pgtable iop;
	int pgd_bits;
	int start_level;
	int bits_per_level;
	void *pgd;
	bool ns;
	void *ns_pgd;
};

static void *realm_host_alloc_pages(size_t size, gfp_t gfp,
				    struct io_pgtable_cfg *cfg,
				    void *cookie)
{
	struct device *dev = cfg->iommu_dev;
	int order = get_order(size);
	void *pages;

	if (gfp & __GFP_HIGHMEM)
		return NULL;

	if (cfg->alloc) {
		pages = cfg->alloc(cookie, size, gfp);
	} else {
		struct page *p;

		p = alloc_pages_node(dev_to_node(dev), gfp | __GFP_ZERO, order);
		pages = p ? page_address(p) : NULL;
	}

	if (!pages)
		return NULL;

	return pages;
}

static struct realm_io_pgtable *realm_alloc_pgtable(struct io_pgtable_cfg *cfg,
						    bool ns)
{
	struct realm_io_pgtable *data;
	int levels, va_bits, pg_shift;

	cfg->pgsize_bitmap &= SZ_4K | SZ_2M | SZ_1G;

	if (!(cfg->pgsize_bitmap & SZ_4K))
		return NULL;

	if (cfg->ias > REALM_MAX_ADDR_BITS)
		return NULL;

	if (cfg->oas > REALM_MAX_ADDR_BITS)
		return NULL;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return NULL;
	data->ns = ns;

	pg_shift = __ffs(cfg->pgsize_bitmap);
	data->bits_per_level = pg_shift - ilog2(sizeof(realm_iopte));

	va_bits = cfg->ias - pg_shift;
	levels = DIV_ROUND_UP(va_bits, data->bits_per_level);
	data->start_level = REALM_MAX_LEVELS - levels;

	/* Calculate the actual size of our pgd (without concatenation) */
	data->pgd_bits = va_bits - (data->bits_per_level * (levels - 1));

	data->iop.ops = (struct io_pgtable_ops) {
		.map_pages	= NULL,
		.unmap_pages	= NULL,
		.iova_to_phys	= NULL,
	};

	return data;
}

static struct io_pgtable *__realm_alloc_pgtable_s2(struct io_pgtable_cfg *cfg,
						   void *cookie, bool ns)
{
	u64 sl;
	struct realm_io_pgtable *data;
	typeof(&cfg->arm_lpae_s2_cfg.vtcr) vtcr = &cfg->arm_lpae_s2_cfg.vtcr;

	data = realm_alloc_pgtable(cfg, ns);
	if (!data)
		return NULL;

	/*
	 * Concatenate PGDs at level 1 if possible in order to reduce
	 * the depth of the stage-2 walk.
	 */
	if (data->start_level == 0) {
		unsigned long pgd_pages;

		pgd_pages = REALM_PGD_SIZE(data) / sizeof(realm_iopte);
		if (pgd_pages <= REALM_S2_MAX_CONCAT_PAGES) {
			data->pgd_bits += data->bits_per_level;
			data->start_level++;
		}
	}

	/* VTCR */
	vtcr->sh = ARM_LPAE_TCR_SH_IS;
	vtcr->irgn = ARM_LPAE_TCR_RGN_WBWA;
	vtcr->orgn = ARM_LPAE_TCR_RGN_WBWA;
	vtcr->tg = ARM_LPAE_TCR_TG0_4K;
	sl = data->start_level + 1;

	switch (cfg->oas) {
	case 32:
		vtcr->ps = ARM_LPAE_TCR_PS_32_BIT;
		break;
	case 36:
		vtcr->ps = ARM_LPAE_TCR_PS_36_BIT;
		break;
	case 40:
		vtcr->ps = ARM_LPAE_TCR_PS_40_BIT;
		break;
	case 42:
		vtcr->ps = ARM_LPAE_TCR_PS_42_BIT;
		break;
	case 44:
		vtcr->ps = ARM_LPAE_TCR_PS_44_BIT;
		break;
	case 48:
		vtcr->ps = ARM_LPAE_TCR_PS_48_BIT;
		break;
	case 52:
		vtcr->ps = ARM_LPAE_TCR_PS_52_BIT;
		break;
	default:
		goto out_free_data;
	}

	vtcr->tsz = 64ULL - cfg->ias;
	vtcr->sl = ~sl & REALM_VTCR_SL0_MASK;

	/* Ensure the empty pgd is visible before any actual TTBR write */
	wmb();

	data->pgd = realm_host_alloc_pages(REALM_PGD_SIZE(data), GFP_KERNEL,
					   cfg, cookie);
	if (!data->pgd)
		goto out_free_data;

	/* TTBR */
	cfg->arm_lpae_s2_cfg.vttbr = virt_to_phys(data->pgd);
	return &data->iop;

out_free_data:
	kfree(data);
	return NULL;
}

static void realm_free_pgtable(struct io_pgtable *iop)
{
	struct realm_io_pgtable *data = io_pgtable_to_data(iop);

	kfree(data);
}

static struct io_pgtable *
realm_alloc_pgtable_s2(struct io_pgtable_cfg *cfg, void *cookie)
{
	return __realm_alloc_pgtable_s2(cfg, cookie, false);
}

struct io_pgtable_init_fns io_pgtable_realm_s2_init_fns = {
	.alloc	= realm_alloc_pgtable_s2,
	.free	= realm_free_pgtable,
};

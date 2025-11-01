/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _GMEM_INTERNAL_H
#define _GMEM_INTERNAL_H

#include <linux/gmem.h>

#ifdef CONFIG_GMEM

void gm_free_page(struct gm_page *gm_page);

static inline void get_gm_page(struct gm_page *gm_page)
{
	atomic_inc(&gm_page->refcount);
}

static inline void put_gm_page(struct gm_page *gm_page)
{
	if (atomic_dec_and_test(&gm_page->refcount))
		gm_free_page(gm_page);
}

int __init gm_page_cachep_init(void);
void gm_page_cachep_destroy(void);

#define GM_MAPPING_CPU		0x10 /* peer-shared page in gm_mapping is on host side */
#define GM_MAPPING_DEVICE	0x20 /* peer-shared page in gm_mapping is on device side */
#define GM_MAPPING_NOMAP	0x40 /* no peer-shared page in gm_mapping */

#define GM_MAPPING_TYPE_MASK	(GM_MAPPING_CPU | GM_MAPPING_DEVICE | GM_MAPPING_NOMAP)


static inline void gm_mapping_flags_set(struct gm_mapping *gm_mapping, int flags)
{
	if (flags & GM_MAPPING_TYPE_MASK)
		gm_mapping->flag &= ~GM_MAPPING_TYPE_MASK;

	gm_mapping->flag |= flags;
}

static inline void gm_mapping_flags_clear(struct gm_mapping *gm_mapping, int flags)
{
	gm_mapping->flag &= ~flags;
}

static inline bool gm_mapping_cpu(struct gm_mapping *gm_mapping)
{
	return !!(gm_mapping->flag & GM_MAPPING_CPU);
}

static inline bool gm_mapping_device(struct gm_mapping *gm_mapping)
{
	return !!(gm_mapping->flag & GM_MAPPING_DEVICE);
}

static inline bool gm_mapping_nomap(struct gm_mapping *gm_mapping)
{
	return !!(gm_mapping->flag & GM_MAPPING_NOMAP);
}

#endif /* CONFIG_GMEM */

#endif /* _GMEM_INTERNAL_H */

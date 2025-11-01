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

#endif /* CONFIG_GMEM */

#endif /* _GMEM_INTERNAL_H */

// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, The Linux Foundation. All rights reserved.
 */

#include <asm/kvm_emulate.h>
#include <asm/kvm_rme_ccal.h>
#include <asm/rmi_cmds.h>
#include <asm/stage2_pgtable.h>

#define RMM_PAGE_SHIFT		12
#define RMM_PAGE_SIZE		BIT(RMM_PAGE_SHIFT)

#define RMM_RTT_BLOCK_LEVEL	2
#define RMM_RTT_MAX_LEVEL	3

/* See ARM64_HW_PGTABLE_LEVEL_SHIFT() */
#define RMM_RTT_LEVEL_SHIFT(l)	\
	((RMM_PAGE_SHIFT - 3) * (4 - (l)) + 3)
#define RMM_L2_BLOCK_SIZE	BIT(RMM_RTT_LEVEL_SHIFT(2))
#define RMM_L1_BLOCK_SIZE	BIT(RMM_RTT_LEVEL_SHIFT(1))

#define CCAL_RTT_ENTRY_NUM	512U

static bool pages_are_consecutive(struct page **pages, int num)
{
	for (int i = 1; i < num; i++) {
		if (page_to_phys(pages[i]) - page_to_phys(pages[i - 1])
		    != PAGE_SIZE)
			return false;
	}

	return true;
}

void config_realm_ccal(struct realm *realm)
{
	realm->params->flags |= RMI_REALM_PARAM_FLAG_CCAL;
	realm->is_ccal = true;
}

static int ccal_create_data_page_unknown(struct realm *realm, unsigned long ipa,
					 struct page *page)
{
	phys_addr_t rd = virt_to_phys(realm->rd);
	phys_addr_t phys = page_to_phys(page);
	int ret, offset;

	for (offset = 0; offset < PAGE_SIZE; offset += RMM_PAGE_SIZE) {
		if (rmi_granule_delegate(phys)) {
			/*
			 * It's likely we raced with another VCPU on the same
			 * fault. Assume the other VCPU has handled the fault
			 * and return to the guest.
			 */
			return 0;
		}

		ret = rmi_data_create_unknown(rd, phys, ipa);

		if (RMI_RETURN_STATUS(ret) == RMI_ERROR_RTT) {
			/* Create missing RTTs and retry */
			int level = RMI_RETURN_INDEX(ret);

			WARN_ON(level == RMM_RTT_MAX_LEVEL);

			ret = realm_create_rtt_levels(realm, ipa, level,
						      RMM_RTT_MAX_LEVEL,
						      NULL);
			if (ret)
				goto err_undelegate;

			ret = rmi_data_create_unknown(rd, phys, ipa);
		}

		if (WARN_ON(ret))
			goto err_undelegate;

		phys += RMM_PAGE_SIZE;
		ipa += RMM_PAGE_SIZE;
	}

	return 0;

err_undelegate:
	if (WARN_ON(rmi_granule_undelegate(phys))) {
		/* Page can't be returned to NS world so is lost */
		get_page(phys_to_page(phys));
	}

	while (offset > 0) {
		unsigned long data, top;

		phys -= RMM_PAGE_SIZE;
		offset -= RMM_PAGE_SIZE;
		ipa -= RMM_PAGE_SIZE;

		WARN_ON(rmi_data_destroy(rd, ipa, &data, &top));

		if (WARN_ON(rmi_granule_undelegate(phys))) {
			/* Page can't be returned to NS world so is lost */
			get_page(phys_to_page(phys));
		}
	}
	return -ENXIO;
}

static int ccal_create_data_block(struct realm *realm, unsigned long ipa,
				  struct page **dst_pages,
				  struct page *tmp_block, unsigned long flags)
{
	phys_addr_t dst_phys, tmp_phys;
	int ret;

	memcpy(page_address(tmp_block), page_address(dst_pages[0]),
	       RMM_L2_BLOCK_SIZE);

	dst_phys = page_to_phys(dst_pages[0]);
	tmp_phys = page_to_phys(tmp_block);

	if (rmi_ccal_delegate_range(dst_phys, RMM_L2_BLOCK_SIZE) != RMI_SUCCESS)
		return -ENXIO;

	ret = rmi_ccal_block_create(virt_to_phys(realm->rd), dst_phys, ipa,
				    tmp_phys, flags);
	if (RMI_RETURN_STATUS(ret) == RMI_ERROR_RTT) {
		/* Create missing RTTs and retry. */
		int err_level = RMI_RETURN_INDEX(ret);

		ret = realm_create_rtt_levels(realm, ipa, err_level,
					      RMM_RTT_BLOCK_LEVEL, NULL);
		if (ret)
			goto err_undelegate;

		ret = rmi_ccal_block_create(virt_to_phys(realm->rd), dst_phys,
					    ipa, tmp_phys, flags);
	}

	if (ret)
		goto err_undelegate;

	return 0;

err_undelegate:
	if (WARN_ON(rmi_ccal_undelegate_range(dst_phys, RMM_L2_BLOCK_SIZE))) {
		for (int i = 0, offset = 0; offset < RMM_L2_BLOCK_SIZE;
		     i++, offset += PAGE_SIZE) {
			/* Pages can't be returned to NS world so are lost. */
			get_page(dst_pages[i]);
		}
	}
	return -ENXIO;
}

static int ccal_create_data_block_unknown(struct realm *realm,
					  struct page **dst_pages,
					  unsigned long ipa)
{
	phys_addr_t dst_phys;
	int ret;

	dst_phys = page_to_phys(dst_pages[0]);

	if (rmi_ccal_delegate_range(dst_phys, RMM_L2_BLOCK_SIZE)) {
		/* Race with another thread. */
		return 0;
	}

	ret = rmi_ccal_block_create_unknown(virt_to_phys(realm->rd), dst_phys,
					    ipa);
	if (RMI_RETURN_STATUS(ret) == RMI_ERROR_RTT) {
		/* Create missing RTTs and retry. */
		int err_level = RMI_RETURN_INDEX(ret);

		ret = realm_create_rtt_levels(realm, ipa, err_level,
					      RMM_RTT_BLOCK_LEVEL, NULL);
		if (ret)
			goto err_undelegate;

		ret = rmi_ccal_block_create_unknown(virt_to_phys(realm->rd),
						    dst_phys, ipa);
	}
	if (ret)
		goto err_undelegate;

	return 0;

err_undelegate:
	if (WARN_ON(rmi_ccal_undelegate_range(dst_phys, RMM_L2_BLOCK_SIZE))) {
		for (int i = 0, offset = 0; offset < RMM_L2_BLOCK_SIZE;
		     i++, offset += PAGE_SIZE) {
			/* Pages can't be returned to NS world so are lost. */
			get_page(dst_pages[i]);
		}
	}

	return -ENXIO;
}

int realm_ccal_populate_region(struct kvm *kvm, phys_addr_t ipa_base,
			       phys_addr_t ipa_end, phys_addr_t *ipa_top,
			       u32 flags)
{
	struct realm *realm = &kvm->arch.realm;
	struct kvm_memory_slot *memslot;
	struct page *tmp_pages = NULL;
	unsigned long data_flags = 0;
	gfn_t base_gfn, top_gfn;
	int nr_pages, nr_pinned;
	struct page **pages;
	unsigned int order;
	unsigned long hva;
	bool block_map;
	int idx;
	int ret;

	if (ipa_base == ipa_end)
		return 0;

	if (flags & KVM_ARM_RME_POPULATE_FLAGS_MEASURE)
		data_flags = RMI_MEASURE_CONTENT;

	if (ipa_base == ALIGN_DOWN(ipa_base, RMM_L2_BLOCK_SIZE) &&
	    ipa_end - ipa_base >= RMM_L2_BLOCK_SIZE) {
		*ipa_top = ipa_base + RMM_L2_BLOCK_SIZE;
		block_map = true;
	} else {
		*ipa_top = min(ipa_end, ALIGN_DOWN(ipa_base + RMM_L2_BLOCK_SIZE,
						   RMM_L2_BLOCK_SIZE));
		block_map = false;
	}

	base_gfn = gpa_to_gfn(ipa_base);
	top_gfn = gpa_to_gfn(*ipa_top);
	nr_pages = top_gfn - base_gfn;

	idx = srcu_read_lock(&kvm->srcu);
	memslot = gfn_to_memslot(kvm, base_gfn);
	if (!memslot) {
		ret = -EFAULT;
		goto out_srcu;
	}

	/* We require the region to be contained within a single memslot. */
	if (memslot->base_gfn + memslot->npages < top_gfn) {
		ret = -EINVAL;
		goto out_srcu;
	}

	hva = gfn_to_hva_memslot(memslot, gpa_to_gfn(ipa_base));
	if (kvm_is_error_hva(hva)) {
		ret = -EINVAL;
		goto out_srcu;
	}

	pages = kmalloc(CCAL_RTT_ENTRY_NUM * sizeof(*pages), GFP_KERNEL);
	if (!pages) {
		ret = -ENOMEM;
		goto out_srcu;
	}

	nr_pinned = pin_user_pages_fast(hva, nr_pages, FOLL_WRITE, pages);
	if (nr_pinned != nr_pages) {
		ret = -EFAULT;
		goto out_pin;
	}

	if (block_map && !IS_ALIGNED(page_to_phys(pages[0]), RMM_L2_BLOCK_SIZE))
		block_map = false;

	if (block_map && !pages_are_consecutive(pages, nr_pinned))
		block_map = false;

	if (block_map)
		order = get_order(RMM_L2_BLOCK_SIZE);
	else
		order = get_order(RMM_PAGE_SIZE);

	tmp_pages = alloc_pages(GFP_KERNEL, order);
	if (!tmp_pages) {
		ret = -ENOMEM;
		goto out_pin;
	}

	if (block_map) {
		ret = ccal_create_data_block(realm, ipa_base, pages, tmp_pages,
					     data_flags);
		if (ALIGN(ipa_base, RMM_L1_BLOCK_SIZE) ==
		    (ipa_base + RMM_L2_BLOCK_SIZE))
			fold_rtt(realm, ALIGN_DOWN(ipa_base, RMM_L1_BLOCK_SIZE),
				 RMM_RTT_BLOCK_LEVEL);
	} else {
		for (int i = 0; i < nr_pinned; i++) {
			ret = realm_create_protected_data_page(realm, ipa_base,
							       pages[i],
							       tmp_pages,
							       data_flags);
			if (ret)
				break;
			ipa_base += RMM_PAGE_SIZE;
		}
	}

	if (ret == 0)
		goto out_free;
out_pin:
	unpin_user_pages(pages, nr_pinned);
out_free:
	kfree(pages);
	if (tmp_pages)
		__free_pages(tmp_pages, order);
out_srcu:
	srcu_read_unlock(&kvm->srcu, idx);
	return ret;
}

static int ccal_map_range(struct kvm *kvm, unsigned long ipa_base,
			  unsigned long ipa_top)
{
	struct realm *realm = &kvm->arch.realm;
	struct kvm_memory_slot *memslot;
	gfn_t base_gfn, top_gfn;
	int nr_pages, nr_pinned;
	struct page **pages;
	unsigned long hva;
	bool block_map;
	int idx;
	int ret;

	if (IS_ALIGNED(ipa_base, RMM_L2_BLOCK_SIZE) &&
	    (ipa_top - ipa_base == RMM_L2_BLOCK_SIZE))
		block_map = true;
	else
		block_map = false;

	base_gfn = gpa_to_gfn(ipa_base);
	top_gfn = gpa_to_gfn(ipa_top);

	nr_pages = top_gfn - base_gfn;

	idx = srcu_read_lock(&kvm->srcu);
	memslot = gfn_to_memslot(kvm, base_gfn);
	if (!memslot) {
		ret = -EFAULT;
		goto out_srcu;
	}

	/* We require the region to be contained within a single memslot. */
	if (memslot->base_gfn + memslot->npages < top_gfn) {
		ret = -EFAULT;
		goto out_srcu;
	}

	pages = kmalloc(CCAL_RTT_ENTRY_NUM * sizeof(*pages), GFP_KERNEL);
	if (!pages) {
		ret = -ENOMEM;
		goto out_srcu;
	}

	hva = gfn_to_hva_memslot(memslot, gpa_to_gfn(ipa_base));
	nr_pinned = pin_user_pages_fast(hva, nr_pages, FOLL_WRITE, pages);
	if (nr_pinned != nr_pages) {
		ret = -EFAULT;
		goto out_pin;
	}

	if (block_map && !IS_ALIGNED(page_to_phys(pages[0]), RMM_L2_BLOCK_SIZE))
		block_map = false;

	if (block_map && !pages_are_consecutive(pages, nr_pinned))
		block_map = false;

	if (block_map) {
		ret = ccal_create_data_block_unknown(realm, pages, ipa_base);
		if (ALIGN(ipa_base, RMM_L1_BLOCK_SIZE) ==
		    (ipa_base + RMM_L2_BLOCK_SIZE))
			fold_rtt(realm, ALIGN_DOWN(ipa_base, RMM_L1_BLOCK_SIZE),
				 RMM_RTT_BLOCK_LEVEL);
	} else {
		for (int i = 0; i < nr_pinned; i++) {
			ret = ccal_create_data_page_unknown(realm, ipa_base,
							    pages[i]);
			if (ret)
				break;

			ipa_base += RMM_PAGE_SIZE;
		}
	}

	if (ret == 0)
		goto out_free;
out_pin:
	unpin_user_pages(pages, nr_pinned);
out_free:
	kfree(pages);
out_srcu:
	srcu_read_unlock(&kvm->srcu, idx);
	return ret;
}

int realm_ccal_map_ram(struct kvm *kvm,
		       struct arm_rme_populate_realm *args)
{
	phys_addr_t ipa_base, ipa_end, next_ipa;
	int ret;

	if (kvm_realm_state(kvm) != REALM_STATE_NEW)
		return -EINVAL;

	ipa_base = args->base;
	ipa_end = ipa_base + args->size;

	if (!IS_ALIGNED(ipa_base, PAGE_SIZE) ||
	    !IS_ALIGNED(ipa_end, PAGE_SIZE) ||
	    ipa_base > ipa_end)
		return -EINVAL;

	if (ipa_base == ipa_end)
		return 0;

	while (ipa_base < ipa_end) {
		next_ipa = min(ipa_end, ALIGN_DOWN(ipa_base + RMM_L2_BLOCK_SIZE,
						   RMM_L2_BLOCK_SIZE));
		ret = ccal_map_range(kvm, ipa_base, next_ipa);
		if (ret)
			break;

		ipa_base = next_ipa;
		cond_resched();
	}

	return ret;
}

static int ccal_destroy_data(struct realm *realm, unsigned long ipa,
			     unsigned long *next_addr)
{
	unsigned long pa, size, granule_type, offset;
	unsigned long rd = virt_to_phys(realm->rd);
	int ret;

	ret = rmi_ccal_data_destroy(rd, ipa, &pa, &size, &granule_type,
				    next_addr);

	if (WARN_ON(ret))
		return -ENXIO;

	if (granule_type == CCAL_GRANULE_NORMAL)
		ret = rmi_ccal_undelegate_range(pa, size);
	else
		goto out_unpin;

	/*
	 * If the undelegate fails then something has gone seriously
	 * wrong: take an extra reference to just leak pages.
	 */
	if (WARN_ON(ret)) {
		for (offset = 0; offset < size; offset += PAGE_SIZE)
			get_page(phys_to_page(pa + offset));
	}

out_unpin:
	for (offset = 0; offset < size; offset += PAGE_SIZE)
		unpin_user_page(phys_to_page(pa + offset));

	return 0;
}

void realm_ccal_destroy_data_range(struct kvm *kvm, unsigned long start,
				   unsigned long end)
{
	struct realm *realm = &kvm->arch.realm;
	unsigned long next_addr, addr;
	int ret;

	for (addr = start; addr < end; addr = next_addr) {
		ret = ccal_destroy_data(realm, addr, &next_addr);
		if (ret)
			break;
		cond_resched_rwlock_write(&kvm->mmu_lock);
	}
}

static int ccal_rtt_complement(struct realm *realm, unsigned long ipa,
			       int walk_level, int level)
{
	/*
	 * Walk level could be -1 if the LPA2 feature is enabled.
	 * RMM adds 1 to both of protected level and unprotected level so that
	 * their values can be correctly delieverd.
	 */
	int protected_level = (walk_level & 0xF) - 1;
	int unprotected_level = ((walk_level >> 4) & 0xF) - 1;
	int ret = 0;

	level = max(max(protected_level, unprotected_level), level);
	if (protected_level < level)
		ret = realm_create_rtt_levels(realm, ipa, protected_level,
					      level, NULL);

	if (ret)
		return ret;

	ipa = (1UL << (realm->ia_bits - 1)) | ipa;
	if (unprotected_level < level)
		ret = realm_create_rtt_levels(realm, ipa, unprotected_level,
					      level, NULL);

	return ret;
}

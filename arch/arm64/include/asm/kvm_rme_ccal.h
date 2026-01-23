/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, The Linux Foundation. All rights reserved.
 */

#ifndef __ASM_KVM_RME_CCAL_H
#define __ASM_KVM_RME_CCAL_H

#include <asm/kvm_host.h>
#include <linux/kvm_host.h>

enum CCAL_GRANULE_TYPES {
	CCAL_GRANULE_NORMAL,
	CCAL_GRANULE_DEV,
	CCAL_GRANULE_NS,
	CCAL_GRANULE_TYPES_NUM
};

static inline bool is_ccal_rvm(struct realm *realm)
{
	return realm->is_ccal;
}

void config_realm_ccal(struct realm *realm);

int realm_ccal_populate_region(struct kvm *kvm, phys_addr_t ipa_base,
			       phys_addr_t ipa_end, phys_addr_t *ipa_top,
			       u32 flags);

int realm_ccal_map_ram(struct kvm *kvm,
		       struct arm_rme_populate_realm *args);

void realm_ccal_destroy_data_range(struct kvm *kvm, unsigned long start,
				   unsigned long end);

#endif

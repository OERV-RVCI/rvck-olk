/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, The Linux Foundation. All rights reserved.
 */

#ifndef __ASM_KVM_RME_CCAL_H
#define __ASM_KVM_RME_CCAL_H

#include <asm/kvm_host.h>
#include <linux/kvm_host.h>

static inline bool is_ccal_rvm(struct realm *realm)
{
	return realm->is_ccal;
}

void config_realm_ccal(struct realm *realm);

int realm_ccal_map_ram(struct kvm *kvm,
		       struct arm_rme_populate_realm *args);

#endif

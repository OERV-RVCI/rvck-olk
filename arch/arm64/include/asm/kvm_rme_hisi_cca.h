/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, The Linux Foundation. All rights reserved.
 */

#ifndef __ASM_KVM_RME_HISI_CCA_H
#define __ASM_KVM_RME_HISI_CCA_H
#ifdef CONFIG_HISI_CCA

#include <asm/kvm_host.h>
#include <linux/kvm_host.h>

int rmi_cca_hisi_delegate_range(unsigned long start_addr, unsigned long size);

static inline int rmi_cca_hisi_undelegate_range(unsigned long start_addr,
						unsigned long size)
{
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_HISI_EXT, CCA_HISI_UNDELEGATE_RANGE,
		start_addr, size
	};

	arm_smccc_1_2_smc(&regs, &regs);

	return regs.a0;
}

static inline int rmi_cca_hisi_block_create(unsigned long rd,
					    unsigned long data,
					    unsigned long ipa,
					    unsigned long src,
					    unsigned long flags)
{
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_HISI_EXT, CCA_HISI_BLOCK_DATA_CREATE,
		rd, data, ipa, src, flags
	};

	arm_smccc_1_2_smc(&regs, &regs);

	return regs.a0;
}

static inline int rmi_cca_hisi_block_create_unknown(unsigned long rd,
						    unsigned long data,
						    unsigned long ipa,
						    unsigned long level)
{
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_HISI_EXT, CCA_HISI_BLOCK_DATA_CREATE_UNKNOWN,
		rd, data, ipa, level
	};

	arm_smccc_1_2_smc(&regs, &regs);

	return regs.a0;
}

static inline int rmi_cca_hisi_data_destroy(unsigned long rd, unsigned long ipa,
					    unsigned long *pa,
					    unsigned long *size,
					    unsigned long *granule_type,
					    unsigned long *top_ipa)
{
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_HISI_EXT, CCA_HISI_DATA_DESTROY,
		rd, ipa
	};

	arm_smccc_1_2_smc(&regs, &regs);

	*pa = regs.a1;
	*size = regs.a2;
	*granule_type = regs.a3;
	*top_ipa = regs.a4;

	return regs.a0;
}

int realm_hisi_cca_populate_region(struct kvm *kvm, phys_addr_t ipa_base,
				   phys_addr_t ipa_end, phys_addr_t *ipa_top,
				   u32 flags);

int realm_hisi_cca_map_ram(struct kvm *kvm,
			   struct arm_rme_map_ram_args *args);

void realm_hisi_cca_destroy_data_range(struct kvm *kvm, unsigned long start,
				       unsigned long end);

int realm_hisi_cca_set_ipa_state(struct kvm_vcpu *vcpu, unsigned long start,
				 unsigned long end, unsigned long ripas,
				 unsigned long *top_ipa);

#endif /* CONFIG_HISI_CCA */
#endif

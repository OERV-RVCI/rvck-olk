// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, The Linux Foundation. All rights reserved.
 */

#include <asm/rmi_cmds.h>
#include <asm/hisi_cca_da.h>

#include "arm-smmu-v3.h"
#include "arm-r-smmu-v3.h"

bool arm_smmu_support_rme(struct arm_smmu_device *smmu)
{
	if (!is_support_rme())
		return false;

	return smmu->realm.enabled;
}

void arm_r_smmu_device_init(struct arm_smmu_device *smmu, resource_size_t ioaddr)
{
	int ret;
	u32 idr0;
	struct realm_smmu_device *realm = &smmu->realm;

	if (!is_support_rme() || is_realm_world())
		return;

	ret = rmi_smmu_reg_read32(ioaddr, SMMU_R_IDR0, &idr0);
	if (ret)
		return;

	realm->ioaddr = ioaddr;
	if (idr0 & R_IDR0_MSI)
		realm->support_msi = true;

	if (smmu->features & ARM_SMMU_FEAT_E2H) {
		ret = rmi_smmu_reg_write32(ioaddr, SMMU_R_CR2, CR2_E2H);
		if (ret) {
			dev_err(smmu->dev, "failed to write realm SMMU_R_CR2\n");
			return;
		}
	}

	realm->enabled = true;
}

void arm_r_smmu_device_remove(struct arm_smmu_device *smmu)
{
}

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026, The Linux Foundation. All rights reserved.
 */

#ifndef _ARM_R_SMMU_V3_H
#define _ARM_R_SMMU_V3_H

#define SMMU_R_IDR0			0
#define R_IDR0_MSI			(1U << 13)

#define SMMU_R_CR2			0x2C

bool arm_smmu_support_rme(struct arm_smmu_device *smmu);
void arm_r_smmu_device_init(struct arm_smmu_device *smmu, resource_size_t ioaddr);
void arm_r_smmu_device_remove(struct arm_smmu_device *smmu);

#endif /* _ARM_R_SMMU_V3_H */

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026, The Linux Foundation. All rights reserved.
 */

#ifndef _ARM_R_SMMU_V3_H
#define _ARM_R_SMMU_V3_H

#include <linux/iopoll.h>

#define SMMU_R_IDR0			0
#define R_IDR0_MSI			(1U << 13)

#define SMMU_R_CR0			0x20
#define SMMU_R_CR0ACK			0x24

#define SMMU_R_CR2			0x2C

#define SMMU_R_IRQ_CTRL			0x50
#define SMMU_R_IRQ_CTRLACK		0x54

#define SMMU_R_GERROR			0x60
#define SMMU_R_GERRORN			0x64

#define SMMU_R_GERROR_IRQ_CFG0		0x68
#define SMMU_R_GERROR_IRQ_CFG1		0x70
#define SMMU_R_GERROR_IRQ_CFG2		0x74

#define SMMU_R_EVENTQ_IRQ_CFG0		0xb0
#define SMMU_R_EVENTQ_IRQ_CFG1		0xb8
#define SMMU_R_EVENTQ_IRQ_CFG2		0xbc

#define SMMU_R_CMDQ			0
#define SMMU_R_EVTQ			1

#define MSI_CFG0_NS			(1UL << 63)

#define realm_read_poll_timeout(op, val, cond, delay_us, timeout_us, \
				delay_before_read, args...) \
({ \
	u64 __timeout_us = (timeout_us); \
	int __ret = 0; \
	unsigned long __delay_us = (delay_us); \
	ktime_t __timeout = ktime_add_us(ktime_get(), __timeout_us); \
	if (delay_before_read && __delay_us) \
		udelay(__delay_us); \
	for (;;) { \
		__ret = op(args, &val); \
		if (__ret) \
			break; \
		if (cond) \
			break; \
		if (__timeout_us && \
		    ktime_compare(ktime_get(), __timeout) > 0) { \
			__ret = op(args, &val); \
			break; \
		} \
		if (__delay_us) \
			udelay(__delay_us); \
		cpu_relax(); \
	} \
	__ret ? __ret : ((cond) ? 0 : -ETIMEDOUT); \
})

bool arm_smmu_support_rme(struct arm_smmu_device *smmu);
void arm_r_smmu_device_init(struct arm_smmu_device *smmu, resource_size_t ioaddr);
void arm_r_smmu_device_remove(struct arm_smmu_device *smmu);

#endif /* _ARM_R_SMMU_V3_H */

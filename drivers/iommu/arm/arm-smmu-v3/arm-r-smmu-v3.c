// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, The Linux Foundation. All rights reserved.
 */

#include <linux/pci.h>
#include <asm/rmi_cmds.h>
#include <asm/hisi_cca_da.h>

#include "arm-smmu-v3.h"
#include "arm-r-smmu-v3.h"

#define STRTAB_L1_DESC_DWORDS		1

#define realm_smmu_read_poll_timeout(addr, val, cond, delay_us, timeout_us) \
	realm_read_poll_timeout(rmi_smmu_reg_read32, val, cond, delay_us, \
				timeout_us, false, smmu->realm.ioaddr, addr)

bool arm_smmu_support_rme(struct arm_smmu_device *smmu)
{
	if (!is_support_rme())
		return false;

	return smmu->realm.enabled;
}

static int realm_smmu_write_reg_sync(struct arm_smmu_device *smmu, u32 val,
				     unsigned int reg_off, unsigned int ack_off)
{
	int ret;
	u32 reg;

	ret = rmi_smmu_reg_write32(smmu->realm.ioaddr, reg_off, val);
	if (ret)
		return ret;

	return realm_smmu_read_poll_timeout(ack_off, reg, reg == val, 1,
					    ARM_SMMU_POLL_TIMEOUT_US);
}

static int realm_smmu_init_queue(struct arm_smmu_device *smmu,
				 struct realm_smmu_queue *q, int ent_dwords,
				 int queue_type, const char *name)
{
	__le64 *base;
	size_t qsz;
	int ret;

	do {
		qsz = ((1 << q->max_n_shift) * ent_dwords) << 3;
		base = dma_alloc_coherent(smmu->dev, qsz, &q->base_dma,
					      GFP_KERNEL);
		if (base || qsz < PAGE_SIZE)
			break;

		q->max_n_shift--;
	} while (1);

	if (!base) {
		dev_err(smmu->dev,
			"failed to allocate queue (0x%zx bytes) for %s\n",
			qsz, name);
		return -ENOMEM;
	}

	if (!WARN_ON(q->base_dma & (qsz - 1))) {
		dev_info(smmu->dev, "allocated %u entries for %s\n",
			 1 << q->max_n_shift, name);
	}

	ret = granule_delegate_range(q->base_dma, qsz);
	if (ret)
		goto out_free;

	ret = realm_config_queue(SMMU_QUEUE_INIT, smmu->realm.ioaddr,
				 queue_type, q->base_dma, q->max_n_shift);
	if (ret)
		goto out_undelegate;

	q->base = base;
	return 0;

out_undelegate:
	if (WARN_ON(granule_undelegate_range(q->base_dma, qsz)))
		return ret;
out_free:
	dma_free_coherent(smmu->dev, qsz, q->base, q->base_dma);
	return ret;
}

static int realm_smmu_init_strtab_2lvl(struct arm_smmu_device *smmu)
{
	int ret;
	void *strtab;
	struct realm_smmu_strtab_cfg *cfg = &smmu->realm.strtab_cfg;
	size_t l1size = cfg->num_l1_ents * (STRTAB_L1_DESC_DWORDS << 3);

	strtab = dma_alloc_coherent(smmu->dev, l1size, &cfg->strtab_dma,
				    GFP_KERNEL);
	if (!strtab)
		return -ENOMEM;

	ret = granule_delegate_range(cfg->strtab_dma, l1size);
	if (ret)
		goto out_free_strtab;

	cfg->l1_desc = kcalloc(cfg->num_l1_ents, sizeof(*cfg->l1_desc),
			       GFP_KERNEL);
	if (!cfg->l1_desc) {
		ret = -ENOMEM;
		goto out_undelegate;
	}

	ret = realm_config_strtab(SMMU_STRTAB_INIT, smmu->realm.ioaddr,
				  cfg->strtab_dma, cfg->strtab_base_cfg);
	if (ret) {
		dev_err(smmu->dev, "failed to init realm linear strtab\n");
		goto out_free_l1_desc;
	}

	cfg->strtab.l1_desc = strtab;

	return 0;

out_free_l1_desc:
	kfree(cfg->l1_desc);
out_undelegate:
	if (WARN_ON(granule_undelegate_range(cfg->strtab_dma, l1size)))
		return ret;
out_free_strtab:
	dma_free_coherent(smmu->dev, l1size, strtab, cfg->strtab_dma);
	return ret;
}

static int realm_smmu_init_strtab_linear(struct arm_smmu_device *smmu)
{
	int ret;
	void *strtab;
	struct realm_smmu_strtab_cfg *cfg = &smmu->realm.strtab_cfg;
	size_t l1size = (1 << smmu->sid_bits) * sizeof(cfg->strtab.linear[0]);

	strtab = dma_alloc_coherent(smmu->dev, l1size, &cfg->strtab_dma,
				    GFP_KERNEL);
	if (!strtab)
		return -ENOMEM;

	ret = granule_delegate_range(cfg->strtab_dma, l1size);
	if (ret)
		goto out_free;

	ret = realm_config_strtab(SMMU_STRTAB_INIT, smmu->realm.ioaddr,
				  cfg->strtab_dma, cfg->strtab_base_cfg);
	if (ret) {
		dev_err(smmu->dev, "failed to init realm linear strtab\n");
		goto out_undelegate;
	}

	cfg->strtab.linear = strtab;
	return 0;

out_undelegate:
	if (WARN_ON(granule_undelegate_range(cfg->strtab_dma, l1size)))
		return ret;
out_free:
	dma_free_coherent(smmu->dev, l1size, strtab, cfg->strtab_dma);
	return ret;
}

static int realm_smmu_init_strtab(struct arm_smmu_device *smmu)
{
	int ret;
	u32 reg;
	struct realm_smmu_strtab_cfg *cfg = &smmu->realm.strtab_cfg;

	/* Reuse num_l1_ents and strtab_base_cfg */
	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		cfg->num_l1_ents = smmu->strtab_cfg.l2.num_l1_ents;
		reg = FIELD_PREP(STRTAB_BASE_CFG_FMT,
				 STRTAB_BASE_CFG_FMT_2LVL) |
		      FIELD_PREP(STRTAB_BASE_CFG_LOG2SIZE,
				 ilog2(cfg->num_l1_ents) + STRTAB_SPLIT) |
		      FIELD_PREP(STRTAB_BASE_CFG_SPLIT, STRTAB_SPLIT);
		cfg->strtab_base_cfg = reg;
		ret = realm_smmu_init_strtab_2lvl(smmu);
	} else {
		cfg->num_l1_ents = smmu->strtab_cfg.linear.num_ents;
		reg = FIELD_PREP(STRTAB_BASE_CFG_FMT,
				 STRTAB_BASE_CFG_FMT_LINEAR) |
		      FIELD_PREP(STRTAB_BASE_CFG_LOG2SIZE, smmu->sid_bits);
		cfg->strtab_base_cfg = reg;
		ret = realm_smmu_init_strtab_linear(smmu);
	}

	return ret;
}

void arm_r_smmu_device_init(struct arm_smmu_device *smmu, resource_size_t ioaddr)
{
	int ret;
	u32 idr0, enables = 0;
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

	realm->rcmdq.max_n_shift = smmu->cmdq.q.llq.max_n_shift;
	/* realm cmdq */
	ret = realm_smmu_init_queue(smmu, &realm->rcmdq, CMDQ_ENT_DWORDS,
				    SMMU_R_CMDQ, "rcmdq");
	if (ret)
		goto err_remove_device;

	/* If SMMU support ATS, SMMU-R should enable ATSCHK */
	if (idr0 & IDR0_ATS)
		enables |= CR0_ATSCHK;

	enables |= CR0_CMDQEN;
	ret = realm_smmu_write_reg_sync(smmu, enables, SMMU_R_CR0, SMMU_R_CR0ACK);
	if (ret) {
		dev_err(smmu->dev, "failed to enable realm command queue\n");
		goto err_remove_device;
	}

	realm->revtq.max_n_shift = smmu->evtq.q.llq.max_n_shift;
	/* realm evtq */
	ret = realm_smmu_init_queue(smmu, &realm->revtq, EVTQ_ENT_DWORDS,
				    SMMU_R_EVTQ, "revtq");
	if (ret)
		goto err_remove_device;

	enables |= CR0_EVTQEN;
	ret = realm_smmu_write_reg_sync(smmu, enables, SMMU_R_CR0, SMMU_R_CR0ACK);
	if (ret) {
		dev_err(smmu->dev, "failed to enable realm event queue\n");
		goto err_remove_device;
	}

	ret = realm_smmu_init_strtab(smmu);
	if (ret)
		goto err_remove_device;

	realm->enabled = true;
	return;

err_remove_device:
	arm_r_smmu_device_remove(smmu);
}

static void arm_r_smmu_free_l2_strtab(struct arm_smmu_device *smmu,
				      struct realm_smmu_strtab_cfg *cfg)
{
	size_t size = (1 << STRTAB_SPLIT) * sizeof(struct arm_smmu_ste);
	struct arm_smmu_strtab_l1_desc *desc;
	unsigned int i;

	if (!cfg->l1_desc)
		return;

	for (i = 0; i < cfg->num_l1_ents; i++) {
		desc = &cfg->l1_desc[i];
		if (!desc->l2ptr)
			continue;
		realm_config_strtab_l2(SMMU_STRTAB_L2_DEINIT,
				       smmu->realm.ioaddr, cfg->strtab_dma,
				       i, desc->l2ptr_dma, desc->span);
		if (WARN_ON(granule_undelegate_range(desc->l2ptr_dma, size)))
			continue;
		dma_free_coherent(smmu->dev, size, desc->l2ptr, desc->l2ptr_dma);
	}
	kfree(cfg->l1_desc);
}

void arm_r_smmu_device_remove(struct arm_smmu_device *smmu)
{
	size_t l1size, qsz;
	struct realm_smmu_queue *q;
	unsigned long ioaddr = smmu->realm.ioaddr;
	struct realm_smmu_device *realm = &smmu->realm;
	struct realm_smmu_strtab_cfg *cfg = &realm->strtab_cfg;

	if (!is_support_rme() || is_realm_world())
		return;

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		l1size = cfg->num_l1_ents * (STRTAB_L1_DESC_DWORDS << 3);
		arm_r_smmu_free_l2_strtab(smmu, cfg);
	} else {
		l1size = cfg->num_l1_ents * sizeof(cfg->strtab.linear[0]);
	}

	if (cfg->strtab.linear) {
		realm_config_strtab(SMMU_STRTAB_DEINIT, ioaddr, cfg->strtab_dma,
				    cfg->strtab_base_cfg);
		if (!WARN_ON(granule_undelegate_range(cfg->strtab_dma, l1size)))
			dma_free_coherent(smmu->dev, l1size, cfg->strtab.linear,
					  cfg->strtab_dma);
	}

	if (realm->rcmdq.base) {
		q = &realm->rcmdq;
		realm_config_queue(SMMU_QUEUE_DEINIT, ioaddr, SMMU_R_CMDQ,
				   q->base_dma, q->max_n_shift);
		qsz = ((1 << q->max_n_shift) * CMDQ_ENT_DWORDS) << 3;
		if (!WARN_ON(granule_undelegate_range(q->base_dma, qsz)))
			dma_free_coherent(smmu->dev, qsz, q->base, q->base_dma);
	}

	if (realm->revtq.base) {
		q = &realm->revtq;
		realm_config_queue(SMMU_QUEUE_DEINIT, ioaddr, SMMU_R_EVTQ,
				   q->base_dma, q->max_n_shift);
		qsz = ((1 << q->max_n_shift) * EVTQ_ENT_DWORDS) << 3;
		if (!WARN_ON(granule_undelegate_range(q->base_dma, qsz)))
			dma_free_coherent(smmu->dev, qsz, q->base, q->base_dma);
	}
}

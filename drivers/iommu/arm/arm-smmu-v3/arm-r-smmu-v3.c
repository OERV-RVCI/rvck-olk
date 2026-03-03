// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, The Linux Foundation. All rights reserved.
 */

#include <linux/pci.h>
#include <asm/rmi_cmds.h>
#include <asm/hisi_cca_da.h>
#include <linux/crash_dump.h>

#include "arm-smmu-v3.h"
#include "arm-r-smmu-v3.h"

#define STRTAB_L1_DESC_DWORDS		1

/*
 * Add realm IRQs index based on arm_smmu_msi_index
 * Struct arm_r_smmu_msi_index need keep same as struct arm_smmu_msi_index
 */
enum arm_r_smmu_msi_index {
	EVTQ_MSI_INDEX,
	GERROR_MSI_INDEX,
	PRIQ_MSI_INDEX,
	R_EVTQ_MSI_INDEX,
	R_GERROR_MSI_INDEX,
	ARM_R_SMMU_MAX_MSIS,
};

#define ARM_R_SMMU_MAX_CFGS 0x3

#define realm_smmu_read_poll_timeout(addr, val, cond, delay_us, timeout_us) \
	realm_read_poll_timeout(rmi_smmu_reg_read32, val, cond, delay_us, \
				timeout_us, false, smmu->realm.ioaddr, addr)

/* Add realm IRQs cfg based on arm_smmu_msi_cfg */
static phys_addr_t arm_r_smmu_msi_cfg[ARM_R_SMMU_MAX_MSIS][ARM_R_SMMU_MAX_CFGS] = {
	[R_EVTQ_MSI_INDEX] = {
		SMMU_R_EVENTQ_IRQ_CFG0,
		SMMU_R_EVENTQ_IRQ_CFG1,
		SMMU_R_EVENTQ_IRQ_CFG2,
	},
	[R_GERROR_MSI_INDEX] = {
		SMMU_R_GERROR_IRQ_CFG0,
		SMMU_R_GERROR_IRQ_CFG1,
		SMMU_R_GERROR_IRQ_CFG2,
	},
};

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

static irqreturn_t arm_r_smmu_revtq_thread(int irq, void *dev)
{
	int i, ret;
	struct arm_smmu_device *smmu = dev;
	static DEFINE_RATELIMIT_STATE(rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);
	u64 evt[EVTQ_ENT_DWORDS];
	u8 id;

	do {
		ret = rmi_smmu_read_event(smmu->realm.ioaddr, evt);
		if (ret)
			break;

		id = FIELD_GET(EVTQ_0_ID, evt[0]);
		if (!__ratelimit(&rs))
			continue;

		dev_info(smmu->dev, "event 0x%02x received:\n", id);
		for (i = 0; i < ARRAY_SIZE(evt); ++i)
			dev_info(smmu->dev, "\t0x%016llx\n",
					(unsigned long long)evt[i]);
		cond_resched();
	} while (true);

	return IRQ_HANDLED;
}

static irqreturn_t arm_smmu_realm_gerror_handler(int irq, void *dev)
{
	u32 gerror, gerrorn, active;
	struct arm_smmu_device *smmu = dev;
	int ret;

	ret = rmi_smmu_reg_read32(smmu->realm.ioaddr, SMMU_R_GERROR, &gerror);
	if (ret) {
		dev_err(smmu->dev, "R_SMMU: Cannot read gerror\n");
		return IRQ_HANDLED;
	}

	ret = rmi_smmu_reg_read32(smmu->realm.ioaddr, SMMU_R_GERRORN, &gerrorn);
	if (ret) {
		dev_err(smmu->dev, "R_SMMU: Cannot read gerrorn\n");
		return IRQ_HANDLED;
	}

	active = gerror ^ gerrorn;
	if (!(active & GERROR_ERR_MASK))
		return IRQ_NONE; /* No errors pending */

	dev_warn(smmu->dev,
		 "R_SMMU: unexpected global error reported (0x%08x), this could be serious\n",
		 active);

	if (active & GERROR_SFM_ERR)
		dev_err(smmu->dev, "R_SMMU: device has entered Service Failure Mode!\n");

	if (active & GERROR_MSI_GERROR_ABT_ERR)
		dev_warn(smmu->dev, "R_SMMU: GERROR MSI write aborted\n");

	if (active & GERROR_MSI_PRIQ_ABT_ERR)
		dev_warn(smmu->dev, "R_SMMU: PRIQ MSI write aborted\n");

	if (active & GERROR_MSI_EVTQ_ABT_ERR)
		dev_warn(smmu->dev, "R_SMMU: EVTQ MSI write aborted\n");

	if (active & GERROR_MSI_CMDQ_ABT_ERR)
		dev_warn(smmu->dev, "R_SMMU: CMDQ MSI write aborted\n");

	if (active & GERROR_PRIQ_ABT_ERR)
		dev_err(smmu->dev, "R_SMMU: PRIQ write aborted -- events may have been lost\n");

	if (active & GERROR_EVTQ_ABT_ERR)
		dev_err(smmu->dev, "R_SMMU: EVTQ write aborted -- events may have been lost\n");

	if (active & GERROR_CMDQ_ERR)
		dev_err(smmu->dev, "R_SMMU: cmdq err\n");

	ret = rmi_smmu_reg_write32(smmu->realm.ioaddr, SMMU_R_GERRORN, gerror);
	if (ret)
		dev_err(smmu->dev, "R_SMMU: Cannot write gerrorn\n");

	return IRQ_HANDLED;
}

static void realm_smmu_write_msi_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	struct device *dev = msi_desc_to_dev(desc);
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	u32 mem_attr = ARM_SMMU_MEMATTR_DEVICE_nGnRE;
	phys_addr_t doorbell;
	phys_addr_t *cfg;
	int ret;

	doorbell = (((u64)msg->address_hi) << 32) | msg->address_lo;
	doorbell &= MSI_CFG0_ADDR_MASK;
	doorbell |= MSI_CFG0_NS;

#ifdef CONFIG_ARM_SMMU_V3_PM
	/* Saves the msg (base addr of msi irq) and restores it during resume */
	desc->msg.address_lo = msg->address_lo;
	desc->msg.address_hi = msg->address_hi;
	desc->msg.data = msg->data;
#endif

	if (desc->msi_index != R_EVTQ_MSI_INDEX && desc->msi_index != R_GERROR_MSI_INDEX) {
		dev_err(dev, "Unsupport msi_index : %u\n", desc->msi_index);
		return;
	}

	cfg = arm_r_smmu_msi_cfg[desc->msi_index];

	ret = rmi_smmu_reg_write64(smmu->realm.ioaddr, cfg[0], doorbell);
	if (ret) {
		dev_err(dev, "Unable to write msi doorbell to %#llx\n", cfg[0]);
		return;
	}

	ret = rmi_smmu_reg_write32(smmu->realm.ioaddr, cfg[1], msg->data);
	if (ret) {
		dev_err(dev, "Unable to write msi data to %#llx\n", cfg[1]);
		return;
	}

	ret = rmi_smmu_reg_write32(smmu->realm.ioaddr, cfg[2], mem_attr);
	if (ret) {
		dev_err(dev, "Unable to write msi attr to %#llx\n", cfg[2]);
		return;
	}
}

static void realm_smmu_setup_msis(struct arm_smmu_device *smmu)
{
	int ret;
	struct device *dev = smmu->dev;

	ret = platform_msi_domain_alloc_range_irqs(dev, R_EVTQ_MSI_INDEX,
		R_GERROR_MSI_INDEX, realm_smmu_write_msi_msg);
	if (ret) {
		dev_warn(dev, "R_SMMU: failed to allocate msis\n");
		return;
	}

	smmu->realm.revtq.irq = msi_get_virq(dev, R_EVTQ_MSI_INDEX);
	smmu->realm.rgerr_irq = msi_get_virq(dev, R_GERROR_MSI_INDEX);
}

static int realm_smmu_setup_unique_irqs(struct arm_smmu_device *smmu)
{
	int irq, ret;
	struct realm_smmu_device *realm = &smmu->realm;
	u32 irqen_flags = IRQ_CTRL_EVTQ_IRQEN | IRQ_CTRL_GERROR_IRQEN;

	if (!realm->support_msi)
		return -EINVAL;

	/* Disable IRQs first */
	ret = realm_smmu_write_reg_sync(smmu, 0, SMMU_R_IRQ_CTRL,
					SMMU_R_IRQ_CTRLACK);
	if (ret) {
		dev_err(smmu->dev, "failed to disable realm irqs\n");
		return ret;
	}

	realm_smmu_setup_msis(smmu);

	irq = realm->revtq.irq;
	if (irq) {
		ret = devm_request_threaded_irq(smmu->dev, irq, NULL,
						arm_r_smmu_revtq_thread,
						IRQF_ONESHOT,
						"arm-smmu-v3-revtq", smmu);
		if (ret < 0)
			dev_warn(smmu->dev, "failed to enable evtq irq\n");
	} else {
		dev_warn(smmu->dev, "no revtq irq - events will not be reported!\n");
	}

	irq = realm->rgerr_irq;
	if (irq) {
		ret = devm_request_threaded_irq(smmu->dev, irq, NULL,
						arm_smmu_realm_gerror_handler,
						IRQF_ONESHOT,
						"arm-smmu-v3-rgerror", smmu);
		if (ret < 0)
			dev_warn(smmu->dev, "failed to enable gerror irq\n");
	} else {
		dev_warn(smmu->dev, "no rgerr irq - errors will not be reported!\n");
	}

	/* Enable interrupt generation on the realm smmu */
	ret = realm_smmu_write_reg_sync(smmu, irqen_flags, SMMU_R_IRQ_CTRL,
					SMMU_R_IRQ_CTRLACK);
	if (ret)
		dev_warn(smmu->dev, "failed to enable realm irqs\n");

	return 0;
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

	ret = realm_smmu_setup_unique_irqs(smmu);
	if (ret) {
		dev_err(smmu->dev, "RME failed to setup irqs\n");
		goto err_remove_device;
	}

	if (is_kdump_kernel())
		enables &= ~CR0_EVTQEN;

	/* Enable the SMMU interface */
	enables |= CR0_SMMUEN;

	ret = realm_smmu_write_reg_sync(smmu, enables, SMMU_R_CR0, SMMU_R_CR0ACK);
	if (ret) {
		dev_err(smmu->dev, "failed to enable realm smmu interface\n");
		goto err_remove_device;
	}

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

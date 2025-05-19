// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm_spe.c: Arm Statistical Profiling Extensions support
 * Copyright (c) 2019-2020, Arm Ltd.
 * Copyright (c) 2024-2025, Huawei Technologies Ltd.
 */

#define PMUNAME "mm_spe"
#define DRVNAME PMUNAME "_driver"
#define pr_fmt(fmt) DRVNAME ": " fmt

#include <linux/of_device.h>
#include <linux/perf/arm_pmu.h>
#include <linux/mem_sampling.h>

#include "spe-decoder/arm-spe-decoder.h"
#include "spe-decoder/arm-spe-pkt-decoder.h"
#include "mm_spe.h"

static struct mm_spe *spe;

#define SPE_INIT_FAIL	0
#define SPE_INIT_READY	1
#define SPE_INIT_SUCC	2
static int spe_probe_status = SPE_INIT_FAIL;

#define SPE_PMU_FEAT_FILT_EVT		(1UL << 0)
#define SPE_PMU_FEAT_FILT_TYP		(1UL << 1)
#define SPE_PMU_FEAT_FILT_LAT		(1UL << 2)
#define SPE_PMU_FEAT_ARCH_INST		(1UL << 3)
#define SPE_PMU_FEAT_LDS		(1UL << 4)
#define SPE_PMU_FEAT_ERND		(1UL << 5)
#define SPE_PMU_FEAT_INV_FILT_EVT	(1UL << 6)
#define SPE_PMU_FEAT_DEV_PROBED	(1UL << 63)

DEFINE_PER_CPU(struct mm_spe_buf, per_cpu_spe_buf);

int mm_spe_percpu_buffer_alloc(int cpu)
{
	struct mm_spe_buf *spe_buf = &per_cpu(per_cpu_spe_buf, cpu);
	void *alloc_base;

	if (spe_buf->base && spe_buf->record_base)
		return 0;

	/* alloc spe raw data buffer */
	alloc_base = kzalloc_node(SPE_BUFFER_MAX_SIZE, GFP_KERNEL, cpu_to_node(cpu));
	if (unlikely(!alloc_base)) {
		pr_err("alloc spe raw data buffer failed.\n");
		return -ENOMEM;
	}

	spe_buf->base = alloc_base;

	spe_buf->size = SPE_BUFFER_SIZE;
	spe_buf->cur = alloc_base + SPE_BUFFER_MAX_SIZE - SPE_BUFFER_SIZE;
	spe_buf->period = SPE_SAMPLE_PERIOD;

	/* alloc record buffer */
	spe_buf->record_size = SPE_RECORD_ENTRY_SIZE * SPE_RECORD_BUFFER_MAX_RECORDS;
	spe_buf->record_base = kzalloc_node(spe_buf->record_size, GFP_KERNEL, cpu_to_node(cpu));
	if (unlikely(!spe_buf->record_base)) {
		kfree(alloc_base);
		pr_err("alloc spe record buffer failed.\n");
		return -ENOMEM;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(mm_spe_percpu_buffer_alloc);

int mm_spe_buffer_alloc(void)
{
	int cpu, ret = 0;
	cpumask_t *mask = &spe->supported_cpus;

	for_each_possible_cpu(cpu) {
		if (!cpumask_test_cpu(cpu, mask))
			continue;
		ret = mm_spe_percpu_buffer_alloc(cpu);
		if (ret)
			return ret;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(mm_spe_buffer_alloc);

void mm_spe_percpu_buffer_free(int cpu)
{
	struct mm_spe_buf *spe_buf = &per_cpu(per_cpu_spe_buf, cpu);

	if (!spe_buf->base)
		return;

	kfree(spe_buf->base);
	spe_buf->cur = NULL;
	spe_buf->base = NULL;
	spe_buf->size = 0;

	kfree(spe_buf->record_base);
	spe_buf->record_base = NULL;
	spe_buf->record_size = 0;
}
EXPORT_SYMBOL_GPL(mm_spe_percpu_buffer_free);

void mm_spe_buffer_free(void)
{
	cpumask_t *mask = &spe->supported_cpus;
	int cpu;

	for_each_possible_cpu(cpu) {
		if (!cpumask_test_cpu(cpu, mask))
			continue;
		mm_spe_percpu_buffer_free(cpu);
	}
	spe_probe_status -= 1;
}
EXPORT_SYMBOL_GPL(mm_spe_buffer_free);

static void mm_spe_buffer_init(void)
{
	u64 base, limit;
	struct mm_spe_buf *spe_buf = this_cpu_ptr(&per_cpu_spe_buf);

	if (!spe_buf || !spe_buf->cur || !spe_buf->size) {
		/*
		 * We still need to clear the limit pointer, since the
		 * profiler might only be disabled by virtue of a fault.
		 */
		limit = 0;
		goto out_write_limit;
	}

	base = (u64)spe_buf->cur;
	limit = ((u64)spe_buf->cur + spe_buf->size) | PMBLIMITR_EL1_E;
	write_sysreg_s(base, SYS_PMBPTR_EL1);

out_write_limit:
	write_sysreg_s(limit, SYS_PMBLIMITR_EL1);
}

void mm_spe_add_probe_status(void)
{
	spe_probe_status += 1;
}
EXPORT_SYMBOL_GPL(mm_spe_add_probe_status);

static void mm_spe_disable_and_drain_local(void)
{
	/* Disable profiling at EL0 and EL1 */
	write_sysreg_s(0, SYS_PMSCR_EL1);
	isb();

	/* Drain any buffered data */
	psb_csync();
	dsb(nsh);

	/* Disable the profiling buffer */
	write_sysreg_s(0, SYS_PMBLIMITR_EL1);
	isb();
}

static u64 mm_spe_to_pmsfcr(void)
{
	u64 reg = 0;

	if (spe->load_filter)
		reg |= PMSFCR_EL1_LD;

	if (spe->store_filter)
		reg |= PMSFCR_EL1_ST;

	if (spe->branch_filter)
		reg |= PMSFCR_EL1_B;

	if (reg)
		reg |= PMSFCR_EL1_FT;

	if (spe->event_filter)
		reg |= PMSFCR_EL1_FE;

	if (spe->inv_event_filter)
		reg |= PMSFCR_EL1_FnE;

	if (spe->min_latency)
		reg |= PMSFCR_EL1_FL;

	return reg;
}

static u64 mm_spe_to_pmsevfr(void)
{
	return spe->event_filter;
}

static u64 mm_spe_to_pmsnevfr(void)
{
	return spe->inv_event_filter;
}

static u64 mm_spe_to_pmslatfr(void)
{
	return spe->min_latency;
}

static void mm_spe_sanitise_period(struct mm_spe_buf *spe_buf)
{
	u64 period = spe_buf->period;
	u64 max_period = PMSIRR_EL1_INTERVAL_MASK;

	if (period < spe->min_period)
		period = spe->min_period;
	else if (period > max_period)
		period = max_period;
	else
		period &= max_period;

	spe_buf->period = period;
}

static u64 mm_spe_to_pmsirr(void)
{
	u64 reg = 0;
	struct mm_spe_buf *spe_buf = this_cpu_ptr(&per_cpu_spe_buf);

	mm_spe_sanitise_period(spe_buf);

	if (spe->jitter)
		reg |= 0x1;

	reg |= spe_buf->period << 8;

	return reg;
}

static u64 mm_spe_to_pmscr(void)
{
	u64 reg = 0;

	if (spe->ts_enable)
		reg |= PMSCR_EL1_TS;

	if (spe->pa_enable)
		reg |= PMSCR_EL1_PA;

	if (spe->pct_enable < 0x4)
		reg |= spe->pct_enable << 6;

	if (spe->exclude_user)
		reg |= PMSCR_EL1_E0SPE;

	if (spe->exclude_kernel)
		reg |= PMSCR_EL1_E1SPE;

	if (IS_ENABLED(CONFIG_PID_IN_CONTEXTIDR))
		reg |= PMSCR_EL1_CX;

	return reg;
}

int mm_spe_start(void)
{
	u64 reg;
	int cpu = smp_processor_id();

	if (!cpumask_test_cpu(cpu, &spe->supported_cpus))
		return -ENOENT;

	mm_spe_buffer_init();

	reg = mm_spe_to_pmsfcr();
	write_sysreg_s(reg, SYS_PMSFCR_EL1);

	reg = mm_spe_to_pmsevfr();
	write_sysreg_s(reg, SYS_PMSEVFR_EL1);

	if (spe->features & SPE_PMU_FEAT_INV_FILT_EVT) {
		reg = mm_spe_to_pmsnevfr();
		write_sysreg_s(reg, SYS_PMSNEVFR_EL1);
	}

	reg = mm_spe_to_pmslatfr();

	write_sysreg_s(reg, SYS_PMSLATFR_EL1);

	reg = mm_spe_to_pmsirr();
	write_sysreg_s(reg, SYS_PMSIRR_EL1);
	isb();

	reg = mm_spe_to_pmscr();
	isb();
	write_sysreg_s(reg, SYS_PMSCR_EL1);
	return 0;
}

void mm_spe_continue(void)
{
	int reg;

	mm_spe_buffer_init();

	reg = mm_spe_to_pmscr();

	isb();
	write_sysreg_s(reg, SYS_PMSCR_EL1);
}

void mm_spe_stop(void)
{
	mm_spe_disable_and_drain_local();
}

void mm_spe_decoding(void)
{
	struct mm_spe_buf *spe_buf = this_cpu_ptr(&per_cpu_spe_buf);

	spe_buf->nr_records = 0;
	arm_spe_decode_buf(spe_buf->cur, spe_buf->size);
}

struct mm_spe_buf *mm_spe_getbuf_addr(void)
{
	struct mm_spe_buf *spe_buf = this_cpu_ptr(&per_cpu_spe_buf);

	return spe_buf;
}

int mm_spe_getnum_record(void)
{
	struct mm_spe_buf *spe_buf = this_cpu_ptr(&per_cpu_spe_buf);

	return spe_buf->nr_records;
}

struct mm_spe *mm_spe_get_desc(void)
{
	return spe;
}
EXPORT_SYMBOL_GPL(mm_spe_get_desc);

int mm_spe_enabled(void)
{
	return spe_probe_status == SPE_INIT_SUCC;
}

static const struct of_device_id mm_spe_sample_para_init_tb[] = {
	{ .compatible = "arm,statistical-profiling-extension-v1",
	  .data = (void *)1 },
	{ /* Sentinel */ },
};
MODULE_DEVICE_TABLE(of, mm_spe_sample_para_init_tb);

static const struct platform_device_id mm_spe_match[] = {
	{ ARMV8_SPE_MEM_SAMPLING_PDEV_NAME, 0 },
	{}
};
MODULE_DEVICE_TABLE(platform, mm_spe_match);

static void mm_spe_sample_para_init(void)
{
	spe->sample_period = SPE_SAMPLE_PERIOD;
	spe->jitter = 1;
	spe->load_filter = 1;
	spe->store_filter = 1;
	spe->branch_filter = 0;
	spe->inv_event_filter = 0;
	spe->event_filter = 0x2;

	spe->ts_enable = 0;
	spe->pa_enable = 1;
	spe->pct_enable = 0;

	spe->exclude_user = 1;
	spe->exclude_kernel = 0;

	spe->min_latency = 120;
}

void mm_spe_record_enqueue(struct arm_spe_record *record)
{
	struct mm_spe_buf *spe_buf = this_cpu_ptr(&per_cpu_spe_buf);
	struct mem_sampling_record *record_tail;

	if (spe_buf->nr_records >= SPE_RECORD_BUFFER_MAX_RECORDS) {
		pr_err("nr_records exceeded!\n");
		return;
	}

	record_tail = spe_buf->record_base +
			spe_buf->nr_records * SPE_RECORD_ENTRY_SIZE;
	*record_tail = *(struct mem_sampling_record *)record;
	spe_buf->nr_records++;
}

static int mm_spe_device_probe(struct platform_device *pdev)
{

	struct device *dev;

	/*
	 * If kernelspace is unmapped when running at EL0, then the SPE
	 * buffer will fault and prematurely terminate the AUX session.
	 */
	if (arm64_kernel_unmapped_at_el0()) {
		dev_warn_once(dev, "buffer inaccessible. Try passing \"kpti=off\" on the kernel command line\n");
		return -EPERM;
	}

	if (!pdev) {
		pr_err("pdev is NULL!\n");
		return -ENODEV;
	}

	dev = &pdev->dev;
	if (!dev) {
		pr_err("dev is NULL!\n");
		return -ENODEV;
	}

	spe = devm_kzalloc(dev, sizeof(*spe), GFP_KERNEL);
	if (!spe)
		return -ENOMEM;

	spe->pdev = pdev;
	platform_set_drvdata(pdev, spe);

	mm_spe_sample_para_init();

	mm_spe_add_probe_status();
	return 0;

}

static struct platform_driver mm_spe_driver = {
	.id_table = mm_spe_match,
	.driver	= {
		.name		= DRVNAME,
		.of_match_table	= of_match_ptr(mm_spe_sample_para_init_tb),
		.suppress_bind_attrs = true,
	},
	.probe	= mm_spe_device_probe,
};

static int __init mm_spe_init(void)
{
	return platform_driver_register(&mm_spe_driver);
}

static void __exit arm_spe_exit(void)
{
	platform_driver_unregister(&mm_spe_driver);
}

subsys_initcall(mm_spe_init);

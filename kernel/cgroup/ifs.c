// SPDX-License-Identifier: GPL-2.0-only
/*
 * Interference statistics for cgroup
 *
 * Copyright (C) 2025-2025 Huawei Technologies Co., Ltd
 */

#include <linux/sched/clock.h>
#include "cgroup-internal.h"

/* smt interference */
struct smt_itf {
	u64 total_time; /* total time of all smt interferences */
	u64 enter_time; /* enter time of the most recent smt interference */
	atomic_t lock;
};

struct smt_info {
	struct smt_itf *itf;
	u64 prev_read_time;
	bool is_noidle;
};

static DEFINE_PER_CPU(struct smt_itf, smt_itf);
static DEFINE_PER_CPU(struct smt_info, smt_info);
static DEFINE_PER_CPU_READ_MOSTLY(int, smt_sibling) = -1;

static DEFINE_PER_CPU(struct cgroup_ifs_cpu, cgrp_root_ifs_cpu);
struct cgroup_ifs cgroup_root_ifs = {
	.pcpu = &cgrp_root_ifs_cpu,
};

DEFINE_STATIC_KEY_FALSE(cgrp_ifs_enabled);

#ifdef CONFIG_CGROUP_IFS_DEFAULT_ENABLED
static bool ifs_enable = true;
#else
static bool ifs_enable;
#endif

#ifdef CONFIG_SCHEDSTATS
static bool ifs_sleep_enable;
#endif

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
static bool ifs_irq_enable;
#endif

static int __init setup_ifs(char *str)
{
	return kstrtobool(str, &ifs_enable) == 0;
}
__setup("cgroup_ifs=", setup_ifs);

void cgroup_ifs_set_smt(cpumask_t *sibling)
{
	int cpu;
	int cpuid1 = -1;
	int cpuid2 = -1;
	bool off = false;
	struct smt_itf *itf;

	for_each_cpu(cpu, sibling) {
		if (cpuid1 == -1) {
			cpuid1 = cpu;
		} else if (cpuid2 == -1) {
			cpuid2 = cpu;
		} else {
			*per_cpu_ptr(&smt_sibling, cpu) = -1;
			off = true;
		}
	}

	if (cpuid1 != -1)
		*per_cpu_ptr(&smt_sibling, cpuid1) = off ? -1 : cpuid2;

	if (cpuid2 != -1)
		*per_cpu_ptr(&smt_sibling, cpuid2) = off ? -1 : cpuid1;

	if (cpuid1 != -1 && cpuid2 != -1 && !off) {
		itf = per_cpu_ptr(&smt_itf, cpuid1);
		per_cpu_ptr(&smt_info, cpuid1)->itf = itf;
		per_cpu_ptr(&smt_info, cpuid2)->itf = itf;
	}
}

static void account_smttime(struct cgroup_ifs *ifs, struct smt_info *info,
			    u64 total_time)
{
	u64 delta;

	if (!ifs)
		return;

	delta = total_time - info->prev_read_time;
	if (!delta)
		return;

	info->prev_read_time = total_time;
	cgroup_ifs_account_delta(this_cpu_ptr(ifs->pcpu), IFS_SMT, delta);
}

static inline void ifs_smt_lock(atomic_t *lock)
{
	while (!atomic_cmpxchg_acquire(lock, 0, 1))
		barrier();
}

static inline void ifs_smt_unlock(atomic_t *lock)
{
	atomic_set_release(lock, 0);
}

void cgroup_ifs_account_smttime(struct task_struct *prev,
				struct task_struct *next,
				struct task_struct *idle)
{
	struct smt_info *ci, *si;
	struct smt_itf *itf;
	u64 now, total_time;
	int sibling;
	s64 delta;

	sibling = this_cpu_read(smt_sibling);
	if (sibling == -1 || prev == next)
		return;

	ci = this_cpu_ptr(&smt_info);
	si = per_cpu_ptr(&smt_info, sibling);
	itf = ci->itf;

	total_time = 0;
	now = sched_clock_cpu(smp_processor_id());

	/* idle => non-idle */
	if (prev == idle) {
		ifs_smt_lock(&itf->lock);
		/* if sibling is also noidle, mark smt interference start */
		if (si->is_noidle)
			itf->enter_time = now;
		ci->is_noidle = true;
		ifs_smt_unlock(&itf->lock);
	/* non-idle => idle */
	} else if (next == idle) {
		ifs_smt_lock(&itf->lock);
		if (itf->enter_time) { /* in smt interference */
			delta = now - itf->enter_time;
			if (delta > 0)
				itf->total_time += delta;
			/* leave smt interference */
			itf->enter_time = 0;
		}
		total_time = itf->total_time;
		ci->is_noidle = false;
		ifs_smt_unlock(&itf->lock);

		account_smttime(task_ifs(prev), ci, total_time);
	/* non-idle => non-idle */
	} else {
		ifs_smt_lock(&itf->lock);
		if (itf->enter_time) { /* in smt interference */
			delta = now - itf->enter_time;
			if (delta > 0) {
				itf->total_time += delta;
				itf->enter_time = now;
			}
		}
		total_time = itf->total_time;
		ifs_smt_unlock(&itf->lock);

		account_smttime(task_ifs(prev), ci, total_time);
	}
}

int cgroup_ifs_alloc(struct cgroup *cgrp)
{
	cgrp->ifs = kzalloc(sizeof(struct cgroup_ifs), GFP_KERNEL);
	if (!cgrp->ifs)
		return -ENOMEM;

	cgrp->ifs->pcpu = alloc_percpu(struct cgroup_ifs_cpu);
	if (!cgrp->ifs->pcpu) {
		kfree(cgrp->ifs);
		return -ENOMEM;
	}

	return 0;
}

void cgroup_ifs_free(struct cgroup *cgrp)
{
	free_percpu(cgrp->ifs->pcpu);
	kfree(cgrp->ifs);
}

static const char *ifs_type_name(int type)
{
	char *name = NULL;

	switch (type) {
	case IFS_SMT:
		name = "smt";
		break;
	case IFS_RUNDELAY:
		name = "rundelay";
		break;
	case IFS_WAKELAT:
		name = "wakelat";
		break;
	case IFS_THROTTLE:
		name = "throttle";
		break;
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	case IFS_SOFTIRQ:
		name = "softirq";
		break;
	case IFS_HARDIRQ:
		name = "hardirq";
		break;
#endif
#ifdef CONFIG_SCHEDSTATS
	case IFS_SLEEP:
		name = "sleep";
		break;
#endif
	default:
		break;
	}

	return name;
}

static bool should_print(int type)
{
#ifdef CONFIG_SCHEDSTATS
	if (type == IFS_SLEEP)
		return ifs_sleep_enable;
#endif
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	if (type == IFS_SOFTIRQ || type == IFS_HARDIRQ)
		return ifs_irq_enable;
#endif
	return true;
}

static int print_sum_time(struct cgroup_ifs *ifs, struct seq_file *seq)
{
	u64 time[NR_IFS_TYPES] = { 0 };
	int cpu;
	int i;

	for_each_possible_cpu(cpu) {
		for (i = 0; i < NR_IFS_TYPES; i++) {
			if (!should_print(i))
				continue;
			time[i] += per_cpu_ptr(ifs->pcpu, cpu)->time[i];
		}
	}

	seq_printf(seq, "%-18s%s\n", "Interference", "Total Time (ns)");

	for (i = 0; i < NR_IFS_TYPES; i++) {
		if (!should_print(i))
			continue;
		seq_printf(seq, "%-18s%llu\n", ifs_type_name(i), time[i]);
	}

	return 0;
}

static int cgroup_ifs_show(struct seq_file *seq, void *v)
{
	struct cgroup __maybe_unused *cgrp = seq_css(seq)->cgroup;
	struct cgroup_ifs __maybe_unused *ifs = cgroup_ifs(cgrp);
	int ret;

	if (!ifs) {
		pr_info("cgroup_ino(cgrp) = %ld\n", cgroup_ino(cgrp));
		return -EINVAL;
	}

	ret = print_sum_time(ifs, seq);
	if (ret)
		return ret;

	return 0;
}

static ssize_t cgroup_ifs_write(struct kernfs_open_file *of, char *buf,
				size_t nbytes, loff_t off)
{
	struct cgroup *cgrp = seq_css(of->seq_file)->cgroup;
	struct cgroup_ifs *ifs = cgroup_ifs(cgrp);
	struct cgroup_ifs_cpu *ifsc;
	bool clear;
	int cpu;

	if (!ifs)
		return -EOPNOTSUPP;

	if (kstrtobool(strstrip(buf), &clear) < 0)
		return -EINVAL;

	if (!clear) {
		for_each_possible_cpu(cpu) {
			ifsc = per_cpu_ptr(ifs->pcpu, cpu);
			memset(ifsc->time, 0, sizeof(ifsc->time));
		}
	}

	return nbytes;
}

static struct cftype cgroup_ifs_files[] = {
	{
		.name = "interference.stat",
		.seq_show = cgroup_ifs_show,
		.write = cgroup_ifs_write,
	},
	{ }     /* terminate */
};

struct cftype cgroup_v1_ifs_files[] = {
	{
		.name = "interference.stat",
		.flags = CFTYPE_NO_PREFIX,
		.seq_show = cgroup_ifs_show,
		.write = cgroup_ifs_write,
	},
	{ }     /* terminate */
};

void cgroup_ifs_init(void)
{
	if (!ifs_enable)
		return;

	BUG_ON(cgroup_init_cftypes(NULL, cgroup_ifs_files));

	static_branch_enable(&cgrp_ifs_enabled);
}

int cgroup_ifs_add_files(struct cgroup_subsys_state *css, struct cgroup *cgrp)
{
	int ret = 0;

	if (!cgroup_ifs_enabled())
		return 0;

	ret = cgroup_addrm_files(css, cgrp, cgroup_ifs_files, true);
	if (ret < 0) {
		cgroup_addrm_files(css, cgrp, cgroup_ifs_files, false);
		return ret;
	}

	return 0;
}

void cgroup_ifs_rm_files(struct cgroup_subsys_state *css, struct cgroup *cgrp)
{
	if (cgroup_ifs_enabled())
		cgroup_addrm_files(css, cgrp, cgroup_ifs_files, false);
}

#ifdef CONFIG_SCHEDSTATS
void cgroup_ifs_enable_sleep_account(void)
{
	ifs_sleep_enable = true;
}
#endif

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
void cgroup_ifs_enable_irq_account(void)
{
	ifs_irq_enable = true;
}
#endif

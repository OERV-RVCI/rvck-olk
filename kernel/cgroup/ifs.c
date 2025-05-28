// SPDX-License-Identifier: GPL-2.0-only
/*
 * Interference statistics for cgroup
 *
 * Copyright (C) 2025-2025 Huawei Technologies Co., Ltd
 */

#include <linux/sched/clock.h>
#include "cgroup-internal.h"

/* smt information */
struct smt_info {
	u64 total_time;
	u64 prev_read_time;
	u64 noidle_enter_time;
	bool is_noidle;
};

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
}

static void account_smttime(struct task_struct *task)
{
	u64 delta;
	struct cgroup_ifs *ifs;
	struct smt_info *info;

	ifs = task_ifs(task);
	if (!ifs)
		return;

	info = this_cpu_ptr(&smt_info);

	delta = info->total_time - info->prev_read_time;
	info->prev_read_time = info->total_time;

	cgroup_ifs_account_delta(this_cpu_ptr(ifs->pcpu), IFS_SMT, delta);
}

void cgroup_ifs_account_smttime(struct task_struct *prev,
				struct task_struct *next,
				struct task_struct *idle)
{
	struct smt_info *ci, *si;
	u64 now, delta;
	int sibling;

	sibling = this_cpu_read(smt_sibling);
	if (sibling == -1 || prev == next)
		return;

	ci = this_cpu_ptr(&smt_info);
	si = per_cpu_ptr(&smt_info, sibling);

	now = sched_clock_cpu(smp_processor_id());

	/* leave noidle */
	if (prev != idle && next == idle) {
		ci->is_noidle = false;
		/* account interference time */
		if (ci->noidle_enter_time && si->is_noidle) {
			delta = now - ci->noidle_enter_time;

			ci->total_time += delta;
			si->total_time += delta;

			si->noidle_enter_time = 0;
			ci->noidle_enter_time = 0;

			account_smttime(prev);
		}
	/* enter noidle */
	} else if (prev == idle && next != idle) {
		/* if the sibling is also nonidle, there is smt interference */
		if (si->is_noidle) {
			ci->noidle_enter_time = now;
			si->noidle_enter_time = now;
		}
		ci->is_noidle = true;
	/* cgroup changed */
	} else if (task_ifs(prev) != task_ifs(next))
		account_smttime(prev);
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
	default:
		break;
	}

	return name;
}

static int print_sum_time(struct cgroup_ifs *ifs, struct seq_file *seq)
{
	u64 time[NR_IFS_TYPES] = { 0 };
	int cpu;
	int i;

	for_each_possible_cpu(cpu) {
		for (i = 0; i < NR_IFS_TYPES; i++)
			time[i] += per_cpu_ptr(ifs->pcpu, cpu)->time[i];
	}

	seq_printf(seq, "%-18s%s\n", "Interference", "Total Time (ns)");

	for (i = 0; i < NR_IFS_TYPES; i++)
		seq_printf(seq, "%-18s%llu\n", ifs_type_name(i), time[i]);

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

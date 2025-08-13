// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025 Huawei Technologies Co., Ltd
 */
#include <linux/memcontrol.h>
#include <linux/idr.h>
#include <linux/pid_namespace.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/types.h>
#include <linux/page_counter.h>
#include <linux/seq_file.h>
#include <linux/sched/stat.h>
#include <linux/kernel_stat.h>
#include <linux/time64.h>
#include <linux/timekeeping.h>
#include <linux/time_namespace.h>
#include <linux/btf_ids.h>
#include <linux/bpf.h>

/*
 * Common support kfuncs
 */

__bpf_kfunc struct mem_cgroup *bpf_mem_cgroup_from_task(struct task_struct *p)
{
	return mem_cgroup_from_task(p);
}

/*
 * Loadavg related kfuncs
 */

__bpf_kfunc struct pid_namespace *bpf_task_active_pid_ns(struct task_struct *task)
{
	return task_active_pid_ns(task);
}

__bpf_kfunc u64 bpf_pidns_nr_tasks(struct pid_namespace *ns)
{
	struct pid_iter iter = {};
	u32 nr_running = 0, nr_threads = 0;

	for_each_task_in_pidns(iter, ns) {
		nr_threads++;
		if (task_is_running(iter.task))
			nr_running++;
	}

	return (u64)nr_running << 32 | nr_threads;
}

__bpf_kfunc u32 bpf_pidns_last_pid(struct pid_namespace *ns)
{
	return idr_get_cursor(&ns->idr) - 1;
}

/*
 * Swaps related kfuncs
 */

/*
 * no padding member "_f" compared to struct sysinfo, because sizeof(_f)
 * maybe zero and not supported by bpf_verifier.
 */
struct bpf_sysinfo {
	__kernel_long_t uptime;		/* Seconds since boot */
	__kernel_ulong_t loads[3];	/* 1, 5, and 15 minute load averages */
	__kernel_ulong_t totalram;	/* Total usable main memory size */
	__kernel_ulong_t freeram;	/* Available memory size */
	__kernel_ulong_t sharedram;	/* Amount of shared memory */
	__kernel_ulong_t bufferram;	/* Memory used by buffers */
	__kernel_ulong_t totalswap;	/* Total swap space size */
	__kernel_ulong_t freeswap;	/* swap space still available */
	__u16 procs;			/* Number of current processes */
	__u16 pad;			/* Explicit padding for m68k */
	__kernel_ulong_t totalhigh;	/* Total high memory size */
	__kernel_ulong_t freehigh;	/* Available high memory size */
	__u32 mem_unit;			/* Memory unit size in bytes */
};

__bpf_kfunc void bpf_si_memswinfo(struct bpf_sysinfo *bsi)
{
	struct sysinfo *si = (struct sysinfo *)bsi;

	if (si) {
		si_meminfo(si);
		si_swapinfo(si);
	}
}

__bpf_kfunc unsigned long bpf_page_counter_read(struct page_counter *counter)
{
	return page_counter_read(counter);
}

/*
 * Stat related kfuncs
 */

/* Moving src's content to the end of dst. Reference: seq_vprintf. */
__bpf_kfunc void bpf_seq_file_append(struct seq_file *dst, struct seq_file *src)
{
	/*
	 * ->count: length of content
	 * ->size: available buffer space
	 * i.e. seq_printf(dst, "%s", src->buf)
	 */
	if (dst->count < dst->size)
		if (src->count < dst->size - dst->count) {
			memmove(dst->buf + dst->count, src->buf, src->count);
			dst->count += src->count;
		}
}

__bpf_kfunc void bpf_get_boottime_timens(struct task_struct *tsk, struct timespec64 *boottime)
{
	getboottime64(boottime);
	*boottime = timespec64_sub(*boottime, tsk->nsproxy->time_ns->offsets.boottime);
}

__bpf_kfunc unsigned long bpf_get_total_forks(void)
{
	return total_forks;
}

__bpf_kfunc unsigned int bpf_nr_running(void)
{
	return nr_running();
}

__bpf_kfunc unsigned long long bpf_nr_context_switches(void)
{
	return nr_context_switches();
}

__bpf_kfunc unsigned int bpf_nr_iowait(void)
{
	return nr_iowait();
}

/* Kernel statistics for CPU accounting */
__bpf_kfunc unsigned int bpf_kstat_softirqs_cpu(unsigned int irq, int cpu)
{
	return kstat_softirqs_cpu(irq, cpu);
}

__bpf_kfunc unsigned long bpf_kstat_cpu_irqs_sum(unsigned int cpu)
{
	return kstat_cpu_irqs_sum(cpu);
}

__bpf_kfunc void bpf_kcpustat_cpu_fetch(struct kernel_cpustat *dst, int cpu)
{
	kcpustat_cpu_fetch(dst, cpu);
}

/*
 * Meminfo related kfuncs
 */

__bpf_kfunc unsigned long bpf_mem_file_hugepage(void)
{
	return global_node_page_state(NR_FILE_THPS);
}

__bpf_kfunc unsigned long bpf_mem_file_pmdmapped(void)
{
	return global_node_page_state(NR_FILE_PMDMAPPED);
}

__bpf_kfunc unsigned long bpf_mem_kreclaimable(void)
{
	return global_node_page_state_pages(NR_SLAB_RECLAIMABLE_B) +
		global_node_page_state(NR_KERNEL_MISC_RECLAIMABLE);
}

BTF_SET8_START(bpf_common_kfuncs_ids)
BTF_ID_FLAGS(func, bpf_mem_cgroup_from_task, KF_RET_NULL | KF_RCU)
BTF_ID_FLAGS(func, bpf_task_active_pid_ns, KF_TRUSTED_ARGS)
BTF_ID_FLAGS(func, bpf_pidns_nr_tasks)
BTF_ID_FLAGS(func, bpf_pidns_last_pid)
BTF_ID_FLAGS(func, bpf_si_memswinfo)
BTF_ID_FLAGS(func, bpf_page_counter_read)
BTF_ID_FLAGS(func, bpf_seq_file_append)
BTF_ID_FLAGS(func, bpf_get_boottime_timens)
BTF_ID_FLAGS(func, bpf_get_total_forks)
BTF_ID_FLAGS(func, bpf_nr_running)
BTF_ID_FLAGS(func, bpf_nr_context_switches)
BTF_ID_FLAGS(func, bpf_nr_iowait)
BTF_ID_FLAGS(func, bpf_kstat_softirqs_cpu)
BTF_ID_FLAGS(func, bpf_kstat_cpu_irqs_sum)
BTF_ID_FLAGS(func, bpf_kcpustat_cpu_fetch)
BTF_ID_FLAGS(func, bpf_mem_file_hugepage)
BTF_ID_FLAGS(func, bpf_mem_file_pmdmapped)
BTF_ID_FLAGS(func, bpf_mem_kreclaimable)
BTF_SET8_END(bpf_common_kfuncs_ids)

static const struct btf_kfunc_id_set bpf_common_kfuncs_set = {
	.owner		= THIS_MODULE,
	.set		= &bpf_common_kfuncs_ids,
};

static int __init bpf_common_kfuncs_init(void)
{
	return register_btf_kfunc_id_set(BPF_PROG_TYPE_TRACING,
					 &bpf_common_kfuncs_set);
}
late_initcall(bpf_common_kfuncs_init);

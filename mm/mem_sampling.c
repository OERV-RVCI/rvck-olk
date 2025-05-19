// SPDX-License-Identifier: GPL-2.0-only
/*
 * mem_sampling.c: declare the mem_sampling abstract layer and provide
 * unified pmu sampling for NUMA, DAMON, etc.
 *
 * Sample records are converted to mem_sampling_record, and then
 * mem_sampling_record_captured_cb_type invoke the callbacks to
 * pass the record.
 *
 * Copyright (c) 2024-2025, Huawei Technologies Ltd.
 */

#define pr_fmt(fmt) "mem_sampling: " fmt

#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mem_sampling.h>

#define MEM_SAMPLING_DISABLED		0x0
#define MEM_SAMPLING_NORMAL		0x1

struct mem_sampling_ops_struct mem_sampling_ops;
static int mem_sampling_override __initdata;
static int sysctl_mem_sampling_mode;

/* keep track of who use the SPE */
DEFINE_PER_CPU(enum arm_spe_user_e, arm_spe_user);
EXPORT_PER_CPU_SYMBOL_GPL(arm_spe_user);

enum mem_sampling_saved_state_e {
	MEM_SAMPLING_STATE_ENABLE,
	MEM_SAMPLING_STATE_DISABLE,
	MEM_SAMPLING_STATE_EMPTY,
};
enum mem_sampling_saved_state_e mem_sampling_saved_state = MEM_SAMPLING_STATE_EMPTY;

/*
 * Callbacks should be registered using mem_sampling_record_cb_register()
 * by NUMA, DAMON and etc during their initialisation.
 * Callbacks will be invoked on new hardware pmu records caputured.
 */
typedef void (*mem_sampling_record_cb_type)(struct mem_sampling_record *record);

struct mem_sampling_record_cb_list_entry {
	struct list_head list;
	mem_sampling_record_cb_type cb;
};
LIST_HEAD(mem_sampling_record_cb_list);

void mem_sampling_record_cb_register(mem_sampling_record_cb_type cb)
{
	struct mem_sampling_record_cb_list_entry *cb_entry, *tmp;

	list_for_each_entry_safe(cb_entry, tmp, &mem_sampling_record_cb_list, list) {
		if (cb_entry->cb == cb)
			return;
	}

	cb_entry = kmalloc(sizeof(struct mem_sampling_record_cb_list_entry), GFP_KERNEL);
	if (!cb_entry)
		return;

	cb_entry->cb = cb;
	list_add(&(cb_entry->list), &mem_sampling_record_cb_list);
}

void mem_sampling_record_cb_unregister(mem_sampling_record_cb_type cb)
{
	struct mem_sampling_record_cb_list_entry *cb_entry, *tmp;

	list_for_each_entry_safe(cb_entry, tmp, &mem_sampling_record_cb_list, list) {
		if (cb_entry->cb == cb) {
			list_del(&cb_entry->list);
			kfree(cb_entry);
			return;
		}
	}
}

DEFINE_STATIC_KEY_FALSE(mem_sampling_access_hints);
void mem_sampling_sched_in(struct task_struct *prev, struct task_struct *curr)
{
	if (!static_branch_unlikely(&mem_sampling_access_hints))
		return;

	if (!mem_sampling_ops.sampling_start)
		return;

	if (curr->mm)
		mem_sampling_ops.sampling_start();
	else
		mem_sampling_ops.sampling_stop();
}

void mem_sampling_process(void)
{
	int i, nr_records;
	struct mem_sampling_record *record;
	struct mem_sampling_record *record_base;
	struct mem_sampling_record_cb_list_entry *cb_entry, *tmp;

	mem_sampling_ops.sampling_decoding();

	record_base = (struct mem_sampling_record *)mem_sampling_ops.mm_spe_getbuf_addr();
	nr_records = mem_sampling_ops.mm_spe_getnum_record();

	if (list_empty(&mem_sampling_record_cb_list))
		goto out;

	for (i = 0; i < nr_records; i++) {
		record = record_base + i;
		list_for_each_entry_safe(cb_entry, tmp, &mem_sampling_record_cb_list, list) {
			cb_entry->cb(record);
		}
	}
out:
	/* if mem_sampling_access_hints is set to false, stop sampling */
	if (static_branch_unlikely(&mem_sampling_access_hints))
		mem_sampling_ops.sampling_continue();
	else
		mem_sampling_ops.sampling_stop();
}
EXPORT_SYMBOL_GPL(mem_sampling_process);

static inline enum mem_sampling_type_enum mem_sampling_get_type(void)
{
#ifdef CONFIG_ARM_SPE_MEM_SAMPLING
	return MEM_SAMPLING_ARM_SPE;
#else
	return MEM_SAMPLING_UNSUPPORTED;
#endif
}

static void __set_mem_sampling_state(bool enabled)
{
	if (enabled)
		static_branch_enable(&mem_sampling_access_hints);
	else
		static_branch_disable(&mem_sampling_access_hints);
}

void set_mem_sampling_state(bool enabled)
{
	if (mem_sampling_saved_state != MEM_SAMPLING_STATE_EMPTY) {
		mem_sampling_saved_state = enabled ? MEM_SAMPLING_STATE_ENABLE :
					    MEM_SAMPLING_STATE_DISABLE;
		return;
	}

	if (!mem_sampling_ops.sampling_start || !mm_spe_enabled())
		return;
	if (enabled)
		sysctl_mem_sampling_mode = MEM_SAMPLING_NORMAL;
	else
		sysctl_mem_sampling_mode = MEM_SAMPLING_DISABLED;
	__set_mem_sampling_state(enabled);
}

void mem_sampling_user_switch_process(enum user_switch_type type)
{
	bool state, mm_spe_is_perf_user = false;
	int cpu;

	if (type >= USER_SWITCH_TYPE_MAX) {
		pr_err("user switch type error.\n");
		return;
	}

	for_each_possible_cpu(cpu) {
		if (per_cpu(arm_spe_user, cpu) == SPE_USER_PERF) {
			mm_spe_is_perf_user = true;
			break;
		}
	}

	if (type == USER_SWITCH_AWAY_FROM_MEM_SAMPLING) {
		/* save state only the status when leave mem_sampling for the first time */
		if (mem_sampling_saved_state != MEM_SAMPLING_STATE_EMPTY)
			return;

		if (static_branch_unlikely(&mem_sampling_access_hints))
			mem_sampling_saved_state = MEM_SAMPLING_STATE_ENABLE;
		else
			mem_sampling_saved_state = MEM_SAMPLING_STATE_DISABLE;

		pr_debug("user switch away from mem_sampling, %s is saved, set to disable.\n",
				mem_sampling_saved_state ? "disabled" : "enabled");

		set_mem_sampling_state(false);
	} else {
		/* If the state is not backed up, do not restore it */
		if (mem_sampling_saved_state == MEM_SAMPLING_STATE_EMPTY || mm_spe_is_perf_user)
			return;

		state = (mem_sampling_saved_state == MEM_SAMPLING_STATE_ENABLE) ? true : false;
		set_mem_sampling_state(state);
		mem_sampling_saved_state = MEM_SAMPLING_STATE_EMPTY;

		pr_debug("user switch back to mem_sampling, set to saved %s.\n",
				state ? "enalbe" : "disable");
	}
}
EXPORT_SYMBOL_GPL(mem_sampling_user_switch_process);

#ifdef CONFIG_PROC_SYSCTL
static int proc_mem_sampling_enable(struct ctl_table *table, int write,
			  void *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;
	int err;
	int state = sysctl_mem_sampling_mode;

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	t = *table;
	t.data = &state;
	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;
	if (write)
		set_mem_sampling_state(state);
	return err;
}

static struct ctl_table mem_sampling_sysctls[] = {
	{
		.procname       = "mem_sampling_enable",
		.data           = NULL, /* filled in by handler */
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler   = proc_mem_sampling_enable,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{}
};

static void __init mem_sampling_sysctl_init(void)
{
	register_sysctl_init("kernel", mem_sampling_sysctls);
}
#else
#define mem_sampling_sysctl_init() do { } while (0)
#endif

static void __init check_mem_sampling_enable(void)
{
	bool mem_sampling_default = false;

	/* Parsed by setup_mem_sampling. override == 1 enables, -1 disables */
	if (mem_sampling_override)
		set_mem_sampling_state(mem_sampling_override == 1);
	else
		set_mem_sampling_state(mem_sampling_default);
}

static int __init setup_mem_sampling_enable(char *str)
{
	int ret = 0;

	if (!str)
		goto out;

	if (!strcmp(str, "enable")) {
		mem_sampling_override = 1;
		ret = 1;
	}
out:
	if (!ret)
		pr_warn("Unable to parse mem_sampling=\n");

	return ret;
}
__setup("mem_sampling=", setup_mem_sampling_enable);

static int __init mem_sampling_init(void)
{
	enum mem_sampling_type_enum mem_sampling_type = mem_sampling_get_type();
	int cpu;

	switch (mem_sampling_type) {
	case MEM_SAMPLING_ARM_SPE:
		mem_sampling_ops.sampling_start		= mm_spe_start;
		mem_sampling_ops.sampling_stop		= mm_spe_stop;
		mem_sampling_ops.sampling_continue	= mm_spe_continue;
		mem_sampling_ops.sampling_decoding	= mm_spe_decoding;
		mem_sampling_ops.mm_spe_getbuf_addr	= mm_spe_getbuf_addr;
		mem_sampling_ops.mm_spe_getnum_record	= mm_spe_getnum_record;

		break;

	default:
		pr_info("unsupport hardware pmu type(%d), disable access hint!\n",
			mem_sampling_type);
		set_mem_sampling_state(false);
		return -ENODEV;
	}
	check_mem_sampling_enable();
	mem_sampling_sysctl_init();

	for_each_possible_cpu(cpu)
		per_cpu(arm_spe_user, cpu) = SPE_USER_MEM_SAMPLING;

	return 0;
}
late_initcall(mem_sampling_init);

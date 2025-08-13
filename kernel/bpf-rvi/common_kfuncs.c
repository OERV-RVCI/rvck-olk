// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025 Huawei Technologies Co., Ltd
 */
#include <linux/memcontrol.h>
#include <linux/btf_ids.h>
#include <linux/bpf.h>

/*
 * Common support kfuncs
 */

__bpf_kfunc struct mem_cgroup *bpf_mem_cgroup_from_task(struct task_struct *p)
{
	return mem_cgroup_from_task(p);
}

BTF_SET8_START(bpf_common_kfuncs_ids)
BTF_ID_FLAGS(func, bpf_mem_cgroup_from_task, KF_RET_NULL | KF_RCU)
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

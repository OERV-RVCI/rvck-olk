// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025 Huawei Technologies Co., Ltd */
#include <asm/cpufeature.h>

#include <linux/bpf.h>
#include <linux/btf_ids.h>

bool bpf_arm64_cpu_have_feature(unsigned int num)
{
	return cpu_have_feature(num);
}

BTF_SET8_START(bpf_arm64_kfunc_ids)
BTF_ID_FLAGS(func, bpf_arm64_cpu_have_feature, KF_RCU)
BTF_SET8_END(bpf_arm64_kfunc_ids)

static const struct btf_kfunc_id_set bpf_arm64_kfunc_set = {
	.owner		= THIS_MODULE,
	.set		= &bpf_arm64_kfunc_ids,
};

static int __init bpf_arm64_kfunc_init(void)
{
	return register_btf_kfunc_id_set(BPF_PROG_TYPE_TRACING,
					 &bpf_arm64_kfunc_set);
}
late_initcall(bpf_arm64_kfunc_init);

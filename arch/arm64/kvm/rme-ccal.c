// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, The Linux Foundation. All rights reserved.
 */

#include <asm/kvm_rme_ccal.h>

void config_realm_ccal(struct realm *realm)
{
	realm->params->flags |= RMI_REALM_PARAM_FLAG_CCAL;
	realm->is_ccal = true;
}

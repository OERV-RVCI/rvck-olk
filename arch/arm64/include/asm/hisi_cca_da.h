/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_HISI_CCA_DA_H
#define __ASM_HISI_CCA_DA_H

#include <asm/kvm_host.h>

#define MAX_DEV_PER_PORT 256
struct rmi_dev_delegate_params {
	uint16_t root_bdf; /* BDF of the PCIe root port or KAE PF device */
	uint16_t num_dev; /* number of attachable devices */
	uint32_t rsvd; /* padding for 64-bit alignment */
	uint16_t devs[MAX_DEV_PER_PORT]; /* BDF of each attachable device */
};

bool is_support_rme(void);

#endif

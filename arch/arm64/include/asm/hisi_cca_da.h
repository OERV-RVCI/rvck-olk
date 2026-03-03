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

struct rdev_node {
	uint16_t dev_bdf;
	struct device *dev;
	struct list_head list;
};

struct realm_dev_entry {
	struct device *dev;
	u64 vttbr;
	u64 msi_iova;
	bool realm;
	struct hlist_node node;
};

#define REALM_MSI_ORIG_IOVA		0x8000000

bool is_support_rme(void);

int realm_attach_devs(struct realm *realm);
void realm_destroy_dev_list(struct realm *realm);
int kvm_rme_assign_device(struct pci_dev *pdev, struct kvm *kvm);
void kvm_rme_unassign_device(struct pci_dev *pdev, struct kvm *kvm);
int kvm_arm_vcpu_rme_dev_validate(struct kvm_vcpu *vcpu,
				  struct kvm_arm_rme_dev_validate *args);

struct realm *rme_get_realm(u64 vttbr);
void rme_add_dev_entry(struct device *dev, u64 vttbr, bool realm);
void rme_update_msi_iova(u64 vttbr, u64 msi_iova);
bool rme_is_realm_dev(struct device *dev);
void rme_remove_dev_entry(struct device *dev);

#endif

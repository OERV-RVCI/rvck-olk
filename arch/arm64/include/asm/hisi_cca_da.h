/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_HISI_CCA_DA_H
#define __ASM_HISI_CCA_DA_H

#include <asm/kvm_host.h>
#include "../../../../drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.h"

#define MAX_DEV_PER_PORT 256
struct rmi_dev_delegate_params {
	uint16_t root_bdf; /* BDF of the PCIe root port or KAE PF device */
	uint16_t num_dev; /* number of attachable devices */
	uint32_t rsvd;
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
	u64 ns_vttbr;
	u64 msi_iova;
	bool realm;
	bool pcipc_ns;
	struct hlist_node node;
};

#define REALM_MSI_ORIG_IOVA		0x8000000

bool is_support_rme(void);

int realm_attach_devs(struct realm *realm);
void realm_destroy_dev_list(struct realm *realm);
int kvm_rme_assign_device(struct pci_dev *pdev, struct kvm *kvm);
void kvm_rme_unassign_device(struct pci_dev *pdev, struct kvm *kvm);
void kvm_complete_dev_op(struct kvm_vcpu *vcpu);

void hisi_pcipc_ns_add(const struct pci_device_id *id_table);
void hisi_pcipc_ns_remove(const struct pci_device_id *id_table);
bool is_hisi_pcipc_ns(struct device *dev);

struct realm *rme_get_realm(u64 vttbr);
void rme_add_dev_entry(struct device *dev, u64 vttbr, bool realm, u64 ns_vttbr,
		       bool pcipc_ns);
u64 rme_get_ns_vttbr(struct device *dev);
void rme_update_msi_iova(u64 vttbr, u64 msi_iova);
bool rme_is_realm_dev(struct device *dev);
bool rme_is_pcipc_ns_dev(struct device *dev);
void rme_remove_dev_entry(struct device *dev);

int realm_smmu_init_l2_strtab(struct arm_smmu_device *smmu, u32 sid);

bool rme_dev_pci_read_msi_msg(struct msi_desc *desc, struct msi_msg *msg);
bool rme_dev_pci_write_msg_msi(struct msi_desc *desc, struct msi_msg *msg);
void rme_dev_fix_msi_address(struct msi_desc *desc, struct msi_msg *msg);
bool rme_dev_msix_prepare_msi_desc(struct pci_dev *dev, struct msi_desc *desc);
bool rme_dev_pci_msix_write_vector_ctrl(struct msi_desc *desc, u32 ctrl);
bool rme_dev_pci_msix_mask(struct msi_desc *desc);
bool rme_dev_msix_mask_all(struct pci_dev *dev, int tsize);

u32 readl_hook(void __iomem *addr, struct pci_dev *pdev);
void writel_hook(u32 val, void __iomem *addr, struct pci_dev *pdev);
void __raw_writel_hook(u32 val, void __iomem *addr, struct pci_dev *pdev);

bool is_realm_device(struct device *dev, struct device_driver *drv);
#endif

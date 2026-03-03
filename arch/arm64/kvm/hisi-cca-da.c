// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 */
#include <linux/hashtable.h>
#include <linux/pci.h>
#include <asm/rmi_cmds.h>
#include <asm/kvm_emulate.h>
#include <asm/hisi_cca_da.h>

#define MAX_REALM_DEV_NUM_ORDER		8
#define PCI_DEVICE_ID_HUAWEI_ZIP_PF	0xa250
#define PCI_DEVICE_ID_HUAWEI_SEC_PF	0xa255
#define PCI_DEVICE_ID_HUAWEI_HPRE_PF	0xa258

struct dev_hash_entry {
	u16 root_bdf; /* The bdf of the root device: pcie root port, or acc PF */
	bool delegated; /* True if device is delegated to Realm */
	u32 assigned_cnt; /* The count of devs assigned in VMs */
	struct hlist_node node; /* hash table */
};

static DEFINE_HASHTABLE(g_root_dev_htable, MAX_REALM_DEV_NUM_ORDER);
static DEFINE_SPINLOCK(g_dev_htable_lock);

bool is_support_rme(void)
{
	return static_branch_unlikely(&kvm_rme_is_available);
}
EXPORT_SYMBOL_GPL(is_support_rme);

/**
 * get_child_devices_rec - Traverse pcie topology to find child devices
 * If dev is a bridge, get it's children
 * If dev is a regular device, get itself
 * @dev: Device for which to get child devices
 * @devs: All child devices under input dev
 * @max_devs: Max num of devs
 * @ndev: Num of child devices
 */
static int get_child_devices_rec(struct pci_dev *dev, uint16_t *devs,
				  int max_devs, int *ndev)
{
	struct pci_bus *bus = dev->subordinate;

	if (bus) { /* dev is a bridge */
		struct pci_dev *child;
		int ret = 0;

		list_for_each_entry(child, &bus->devices, bus_list) {
			ret = get_child_devices_rec(child, devs, max_devs, ndev);
			if (ret < 0)
				return ret;
		}
	} else { /* dev is a regular device */
		uint16_t bdf = pci_dev_id(dev);
		int i;
		/* check if bdf is already in devs */
		for (i = 0; i < *ndev; i++) {
			if (devs[i] == bdf)
				return *ndev;
		}
		/* check overflow */
		if (*ndev >= max_devs)
			return -EINVAL;

		devs[*ndev] = bdf;
		*ndev = *ndev + 1;
	}

	return *ndev;
}

static bool is_hisi_acc_dev(struct pci_dev *pdev)
{
	switch (pdev->vendor) {
	case PCI_VENDOR_ID_HUAWEI:
		switch (pdev->device) {
		case PCI_DEVICE_ID_HUAWEI_ZIP_PF:
		case PCI_DEVICE_ID_HUAWEI_SEC_PF:
		case PCI_DEVICE_ID_HUAWEI_HPRE_PF:
		case PCI_DEVICE_ID_HUAWEI_ZIP_VF:
		case PCI_DEVICE_ID_HUAWEI_SEC_VF:
		case PCI_DEVICE_ID_HUAWEI_HPRE_VF:
			return true;
		default:
			return false;
		}
	default:
		return false;
	}
}

static int get_vf_devices(struct pci_dev *pf_dev, uint16_t *devs, int max_devs)
{
	struct pci_dev *vf_dev;
	unsigned short vf_device_id;
	int ndev = 0;

	/* Add PF device */
	devs[ndev] = pci_dev_id(pf_dev);
	ndev++;

	/* Get VF device id */
	if (pf_dev->device == PCI_DEVICE_ID_HUAWEI_ZIP_PF)
		vf_device_id = PCI_DEVICE_ID_HUAWEI_ZIP_VF;
	else if (pf_dev->device == PCI_DEVICE_ID_HUAWEI_SEC_PF)
		vf_device_id = PCI_DEVICE_ID_HUAWEI_SEC_VF;
	else
		vf_device_id = PCI_DEVICE_ID_HUAWEI_HPRE_VF;

	/* Loop through all the VFs to find enabled VFs */
	vf_dev = pci_get_device(pf_dev->vendor, vf_device_id, NULL);
	while (vf_dev) {
		if (vf_dev->is_virtfn && (vf_dev->physfn == pf_dev)) {
			if (ndev >= max_devs)
				return -EINVAL;

			devs[ndev] = pci_dev_id(vf_dev);
			ndev++;
		}
		vf_dev = pci_get_device(pf_dev->vendor, vf_device_id, vf_dev);
	}
	return ndev;
}

/**
 * rme_get_all_dev_info - Retrieve all devices under the root port
 * @rp_dev: root port device
 * @params: Assign device parameters
 *
 * Returns:
 * %0 if get all devices under the root port successful
 * %-EINVAL if the total number of devices under the root port exceeds the maximum
 */
static int rme_get_all_dev_info(struct pci_dev *rp_dev,
				struct rmi_dev_delegate_params *params)
{
	int ret;

	params->root_bdf = pci_dev_id(rp_dev);
	if (is_hisi_acc_dev(rp_dev)) {
		/* Only search if we are a PF */
		if (!rp_dev->is_physfn) {
			pci_err(rp_dev, "Root dev is not a PF\n");
			return -EINVAL;
		}

		ret = get_vf_devices(rp_dev, params->devs, MAX_DEV_PER_PORT);
	} else {
		int ndev = 0;

		ret = get_child_devices_rec(rp_dev, params->devs,
					    MAX_DEV_PER_PORT, &ndev);
	}

	if (ret < 0) {
		pci_err(rp_dev, "Dev nums overflow\n");
		return -EINVAL;
	}
	params->num_dev = (uint16_t)ret;
	return 0;
}

static struct pci_dev *rme_get_root_dev(struct pci_dev *pdev)
{
	if (is_hisi_acc_dev(pdev))
		return pci_physfn(pdev);

	return pcie_find_root_port(pdev);
}

static inline bool is_root_dev_delegated(u16 root_bdf)
{
	struct dev_hash_entry *entry;

	hash_for_each_possible(g_root_dev_htable, entry, node, root_bdf) {
		if (entry->root_bdf == root_bdf && entry->delegated)
			return true;
	}

	return false;
}

static inline bool is_dev_delegated(struct pci_dev *pdev)
{
	struct pci_dev *root_dev;
	unsigned long flags;
	bool is_delegated;

	root_dev = rme_get_root_dev(pdev);
	if (!root_dev)
		return false;

	spin_lock_irqsave(&g_dev_htable_lock, flags);
	is_delegated = is_root_dev_delegated(pci_dev_id(root_dev));
	spin_unlock_irqrestore(&g_dev_htable_lock, flags);

	return is_delegated;
}

static inline struct dev_hash_entry *find_root_dev_entry(u16 root_bdf)
{
	struct dev_hash_entry *entry;

	hash_for_each_possible(g_root_dev_htable, entry, node, root_bdf) {
		if (entry->root_bdf == root_bdf)
			return entry;
	}

	return NULL;
}

static inline struct dev_hash_entry *add_root_dev_entry(u16 root_bdf)
{
	struct dev_hash_entry *entry;

	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return NULL;

	entry->root_bdf = root_bdf;
	entry->delegated = false;
	entry->assigned_cnt = 0;

	hash_add(g_root_dev_htable, &entry->node, root_bdf);

	return entry;
}

static int rme_root_dev_delegate(phys_addr_t params_addr)
{
	unsigned long out_dev_bdf = ~0;
	unsigned long last_dev_bdf = ~0;
	phys_addr_t dev_info_phys;
	void *dev_info;
	int ret;

retry:
	ret = rmi_root_dev_delegate(params_addr, &out_dev_bdf);
	if (ret == RMI_ERROR_DEV_INFO) {
		if (out_dev_bdf == last_dev_bdf)
			return -ENXIO;

		dev_info = (void *)get_zeroed_page(GFP_ATOMIC);
		if (!dev_info) {
			pr_err("Failed to allocate page for dev %#lx\n",
			       out_dev_bdf);
			return -ENOMEM;
		}

		dev_info_phys = virt_to_phys(dev_info);
		if (rmi_granule_delegate(dev_info_phys)) {
			free_page((unsigned long)dev_info);
			return -ENXIO;
		}

		ret = rmi_dev_init(out_dev_bdf, dev_info_phys);
		if (ret && ret != RMI_ERROR_DEV_EXISTS) {
			if (WARN_ON(rmi_granule_undelegate(dev_info_phys))) {
				/* leak the page */
				return -ENXIO;
			}
			free_page((unsigned long)dev_info);
			return -ENXIO;
		}
		last_dev_bdf = out_dev_bdf;
		goto retry;
	} else if (ret) {
		return -ENXIO;
	}

	return 0;
}

static int delegate_root_dev(struct pci_dev *root_dev)
{
	struct rmi_dev_delegate_params *params = NULL;
	int ret;

	params = (struct rmi_dev_delegate_params *)get_zeroed_page(GFP_ATOMIC);
	if (!params)
		return -ENOMEM;

	ret = rme_get_all_dev_info(root_dev, params);
	if (ret) {
		free_page((unsigned long)params);
		return ret;
	}

	ret = rme_root_dev_delegate(virt_to_phys(params));
	if (ret)
		pci_err(root_dev, "Failed to delegate\n");

	free_page((unsigned long)params);
	return ret;
}

/* Delegate root dev and devs under it to realm, undelegation is not supported */
static int dev_assign_to_realm(struct dev_hash_entry *entry, struct pci_dev *root_dev)
{
	int ret = 0;

	if (!entry->delegated && entry->assigned_cnt != 0) {
		pci_err(root_dev, "Dev is in normal vm use\n");
		return -EBUSY;
	}

	if (entry->delegated) {
		entry->assigned_cnt++;
		return 0;
	}

	ret = delegate_root_dev(root_dev);
	if (ret)
		return ret;

	entry->delegated = true;
	entry->assigned_cnt++;
	return 0;
}

static int dev_assign_to_ns(struct dev_hash_entry *entry, struct pci_dev *root_dev)
{
	if (entry->delegated) {
		pci_err(root_dev, "Dev is delegated to realm\n");
		return -EINVAL;
	}

	entry->assigned_cnt++;
	return 0;
}

static int rme_dev_assign(struct pci_dev *root_dev, struct kvm *kvm)
{
	struct dev_hash_entry *entry;
	int ret;

	spin_lock(&g_dev_htable_lock);
	entry = find_root_dev_entry(pci_dev_id(root_dev));
	if (!entry) {
		entry = add_root_dev_entry(pci_dev_id(root_dev));
		if (!entry) {
			spin_unlock(&g_dev_htable_lock);
			return -ENOMEM;
		}
	}

	ret = kvm_is_realm(kvm) ? dev_assign_to_realm(entry, root_dev) :
				  dev_assign_to_ns(entry, root_dev);
	spin_unlock(&g_dev_htable_lock);
	return ret;
}

static void rme_dev_unassign(struct pci_dev *pdev)
{
	struct dev_hash_entry *entry;
	struct pci_dev *root_dev;

	root_dev = rme_get_root_dev(pdev);
	if (!root_dev)
		return;

	spin_lock(&g_dev_htable_lock);
	entry = find_root_dev_entry(pci_dev_id(root_dev));
	if (!entry) {
		spin_unlock(&g_dev_htable_lock);
		return;
	}

	if (entry->assigned_cnt != 0)
		entry->assigned_cnt--;

	if (!entry->delegated && entry->assigned_cnt == 0) {
		hash_del(&entry->node);
		kfree(entry);
	}

	spin_unlock(&g_dev_htable_lock);
}

static int realm_add_dev_to_list(struct pci_dev *pdev, struct kvm *kvm)
{
	struct rdev_node *realm_dev;

	/* Add the dev to list first. Once the RD is created, do rmi_dev_attach */
	realm_dev = kzalloc(sizeof(*realm_dev), GFP_KERNEL);
	if (!realm_dev)
		return -ENOMEM;

	realm_dev->dev_bdf = pci_dev_id(pdev);
	realm_dev->dev = &pdev->dev;
	list_add_tail(&realm_dev->list, &kvm->arch.realm.rdev_list);

	return 0;
}

static void realm_del_dev_from_list(struct pci_dev *pdev, struct kvm *kvm)
{
	struct rdev_node *pos, *n;
	struct realm *realm;

	if (!kvm_is_realm(kvm))
		return;

	realm = &kvm->arch.realm;
	list_for_each_entry_safe(pos, n, &realm->rdev_list, list) {
		if (pos->dev == &pdev->dev) {
			list_del(&pos->list);
			kfree(pos);
			break;
		}
	}
}

static int rme_dev_delegate(struct pci_dev *pdev, struct pci_dev *root_dev)
{
	phys_addr_t dev_info_phys;
	void *dev_info;
	int ret;

	ret = rmi_dev_delegate(pci_dev_id(pdev), pci_dev_id(root_dev));
	/* Handle the case that the device is created after the root port is delegated */
	if (ret == RMI_ERROR_DEV_INFO) {
		dev_info = (void *)get_zeroed_page(GFP_ATOMIC);
		if (!dev_info) {
			pci_err(pdev, "Failed to allocate page for dev\n");
			return -ENOMEM;
		}

		dev_info_phys = virt_to_phys(dev_info);
		if (rmi_granule_delegate(dev_info_phys)) {
			free_page((unsigned long)dev_info);
			return -ENXIO;
		}

		ret = rmi_dev_init(pci_dev_id(pdev), dev_info_phys);
		if (ret && ret != RMI_ERROR_DEV_EXISTS) {
			if (WARN_ON(rmi_granule_undelegate(dev_info_phys))) {
				/* leak the page */
				return -ENXIO;
			}
			free_page((unsigned long)dev_info);
			return -ENXIO;
		}

		ret = rmi_dev_delegate(pci_dev_id(pdev), pci_dev_id(root_dev));
		if (ret)
			return -ENXIO;
	} else if (ret) {
		return -ENXIO;
	}

	return 0;
}

int kvm_rme_assign_device(struct pci_dev *pdev, struct kvm *kvm)
{
	struct pci_dev *root_dev;
	int ret;

	if (!is_support_rme() || !pdev || !kvm)
		return -EINVAL;

	root_dev = rme_get_root_dev(pdev);
	if (!root_dev) {
		/* Integrated PCIe device such as PCIe PMU is not supported to assigned to realm */
		if (kvm_is_realm(kvm))
			return -EINVAL;
		else
			return 0;
	}

	ret = rme_dev_assign(root_dev, kvm);
	if (ret) {
		pci_err(pdev, "Failed to assign dev\n");
		return ret;
	}

	if (!kvm_is_realm(kvm))
		return 0;

	ret = realm_add_dev_to_list(pdev, kvm);
	if (ret) {
		pci_err(pdev, "Failed to add dev list\n");
		rme_dev_unassign(pdev);
		return ret;
	}

	ret = rme_dev_delegate(pdev, root_dev);
	if (ret) {
		pci_err(pdev, "Failed to delegate dev\n");
		realm_del_dev_from_list(pdev, kvm);
		rme_dev_unassign(pdev);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(kvm_rme_assign_device);

void kvm_rme_unassign_device(struct pci_dev *pdev, struct kvm *kvm)
{
	if (!pdev || !kvm || !is_support_rme())
		return;

	/* Rme dev undelegate will be processed in _kvm_destroy_realm */
	if (kvm_is_realm(kvm))
		rmi_dev_detach(pci_dev_id(pdev));

	rme_dev_unassign(pdev);
}
EXPORT_SYMBOL_GPL(kvm_rme_unassign_device);

void realm_destroy_dev_list(struct realm *realm)
{
	struct rdev_node *pos, *n;

	list_for_each_entry_safe(pos, n, &realm->rdev_list, list) {
		WARN_ON(rmi_dev_undelegate(pos->dev_bdf));
		list_del(&pos->list);
		kfree(pos);
	}
}

/* After RD created, call this to do attach dev */
int realm_attach_devs(struct realm *realm)
{
	struct rdev_node *dev;
	phys_addr_t rd;
	int ret;

	if (!realm || !realm->rd)
		return -EINVAL;

	rd = virt_to_phys(realm->rd);

	list_for_each_entry(dev, &realm->rdev_list, list) {
		ret = rmi_dev_attach(dev->dev_bdf, rd);
		if (ret)
			goto err_detach;
	}

	return 0;

err_detach:
	realm_destroy_dev_list(realm);

	return ret;
}

int kvm_arm_vcpu_rme_dev_validate(struct kvm_vcpu *vcpu,
				  struct kvm_arm_rme_dev_validate *args)
{
	if (!vcpu || !args || !_vcpu_is_rec(vcpu))
		return -EINVAL;

	/* Record host pdev bdf in rec_enter, complete dev validate in rmm */
	vcpu->arch.rec->run->enter.vfio_dev = args->vfio_dev;
	vcpu->arch.rec->run->enter.dev_bdf = args->dev_bdf;

	return 0;
}

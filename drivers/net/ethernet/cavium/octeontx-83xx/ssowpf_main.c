/*
 * Copyright (C) 2016 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/pci.h>

#include "sso.h"

#define DRV_NAME "octeontx-ssow"
#define DRV_VERSION "0.1"

static atomic_t ssow_count = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(octeontx_ssow_devices_lock);
static LIST_HEAD(octeontx_ssow_devices);

static void identify(struct ssowpf_vf *vf, u16 domain_id,
		     u16 subdomain_id)
{
	struct mbox_ssow_identify *ident;

	ident = (struct mbox_ssow_identify *)vf->ram_mbox_addr;
	ident->domain_id = domain_id;
	ident->subdomain_id = subdomain_id;
}

static int ssow_pf_destroy_domain(u32 id, u16 domain_id,
				  struct kobject *kobj, char *g_name)
{
	int i, vf_idx = 0, ret = 0;
	struct ssowpf *ssow = NULL;
	struct pci_dev *virtfn;
	struct ssowpf *curr;
	u64 reg;

	spin_lock(&octeontx_ssow_devices_lock);
	list_for_each_entry(curr, &octeontx_ssow_devices, list) {
		if (curr->id == id) {
			ssow = curr;
			break;
		}
	}

	if (!ssow) {
		ret = -ENODEV;
		goto unlock;
	}

	for (i = 0; i < ssow->total_vfs; i++) {
		if (ssow->vf[i].domain.in_use &&
		    ssow->vf[i].domain.domain_id == domain_id) {
			ssow->vf[i].domain.domain_id = 0;
			ssow->vf[i].domain.in_use = 0;

			virtfn = pci_get_domain_bus_and_slot(
					pci_domain_nr(ssow->pdev->bus),
					pci_iov_virtfn_bus(ssow->pdev, i),
					pci_iov_virtfn_devfn(ssow->pdev, i));
			if (virtfn && kobj && g_name)
				sysfs_remove_link_from_group(kobj, g_name,
							     virtfn->dev.kobj.
							     name);
			dev_info(&ssow->pdev->dev,
				 "Free vf[%d] from domain:%d subdomain_id:%d\n",
				 i, ssow->vf[i].domain.domain_id, vf_idx);
			/* sso: clear hws's gmctl register */
			reg = 0;
			reg = SSO_MAP_GMID(1); /* write reset value '1'*/
			ret = sso_pf_set_value(id, SSO_PF_HWSX_GMCTL(i), reg);
			if (ret < 0) {
				ret = -EIO;
				goto unlock;
			}
			vf_idx++;	/* HWS cnt */
			identify(&ssow->vf[i], 0x0, 0x0);
			iounmap(ssow->vf[i].domain.reg_base);
			ssow->vf[i].domain.in_use = false;
		}
	}

unlock:
	ssow->vfs_in_use -= vf_idx;

	spin_unlock(&octeontx_ssow_devices_lock);

	return ret;
}

static int ssow_pf_create_domain(u32 id, u16 domain_id, u32 vf_count,
				 void *master, void *master_data,
				 struct kobject *kobj, char *g_name)
{
	struct ssowpf *ssow = NULL;
	struct ssowpf *curr;
	struct pci_dev *virtfn;
	resource_size_t vf_start;
	u64 i, reg = 0;
	int vf_idx = 0, ret = 0;

	if (!kobj || !g_name)
		return -EINVAL;

	spin_lock(&octeontx_ssow_devices_lock);
	list_for_each_entry(curr, &octeontx_ssow_devices, list) {
		if (curr->id == id) {
			ssow = curr;
			break;
		}
	}

	if (!ssow) {
		ret = -ENODEV;
		goto err_unlock;
	}

	for (i = 0; i < ssow->total_vfs; i++) {
		if (ssow->vf[i].domain.in_use) {
			continue;
		} else {
			virtfn = pci_get_domain_bus_and_slot(
					pci_domain_nr(ssow->pdev->bus),
					pci_iov_virtfn_bus(ssow->pdev, i),
					pci_iov_virtfn_devfn(ssow->pdev, i));
			if (!virtfn)
				break;
			sysfs_add_link_to_group(kobj, g_name,
						&virtfn->dev.kobj,
						virtfn->dev.kobj.name);

			ssow->vf[i].domain.domain_id = domain_id;
			ssow->vf[i].domain.subdomain_id = vf_idx;
			ssow->vf[i].domain.gmid = get_gmid(domain_id);

			ssow->vf[i].domain.in_use = true;
			ssow->vf[i].domain.master = master;
			ssow->vf[i].domain.master_data = master_data;

			reg = 0;
			reg = SSO_MAP_GMID(ssow->vf[i].domain.gmid);
			ret = sso_pf_set_value(id,
					       SSO_PF_HWSX_GMCTL(i),
					       reg);
			if (ret < 0) {
				ret = -EIO;
				goto err_unlock;
			}

			/* Clear out groupmask, have VF enable the groups it
			 * wants
			 */
			ret = sso_pf_set_value(id,
					       SSO_PF_HWSX_SX_GRPMASK(i, 0), 0);
			ret |= sso_pf_set_value(id,
					       SSO_PF_HWSX_SX_GRPMASK(i, 1), 0);
			if (ret < 0) {
				ret = -EIO;
				goto err_unlock;
			}

			ssow->vf[i].ram_mbox_addr =
				ioremap(SSOW_RAM_MBOX(i),
					SSOW_RAM_MBOX_SIZE);
			if (!ssow->vf[i].ram_mbox_addr) {
				ret = -ENOMEM;
				goto err_unlock;
			}
			vf_start = SSOW_VF_BASE(i);
			ssow->vf[i].domain.reg_base =
				ioremap(vf_start, SSOW_VF_SIZE);
			if (!ssow->vf[i].domain.reg_base) {
				ret = -ENOMEM;
				goto err_unlock;
			}

			identify(&ssow->vf[i], domain_id, vf_idx);
			vf_idx++;
			if (vf_idx == vf_count) {
				ssow->vfs_in_use += vf_count;
				ret = 0;
				break;
			}
		}
	}

	if (vf_idx != vf_count) {
		ret = -ENODEV;
		goto err_unlock;
	}

	spin_unlock(&octeontx_ssow_devices_lock);
	return ret;

err_unlock:
	spin_unlock(&octeontx_ssow_devices_lock);
	ssow_pf_destroy_domain(id, domain_id, kobj, g_name);
	return ret;
}

static int ssow_pf_get_ram_mbox_addr(u32 node, u16 domain_id,
				     void **ram_mbox_addr)
{
	struct ssowpf *ssow = NULL;
	struct ssowpf *curr;
	u64 i;

	spin_lock(&octeontx_ssow_devices_lock);
	list_for_each_entry(curr, &octeontx_ssow_devices, list) {
		if (curr->id == node) {
			ssow = curr;
			break;
		}
	}

	if (!ssow) {
		spin_unlock(&octeontx_ssow_devices_lock);
		return -ENODEV;
	}

	for (i = 0; i < ssow->total_vfs; i++) {
		if (ssow->vf[i].domain.in_use &&
		    ssow->vf[i].domain.domain_id == domain_id &&
		    ssow->vf[i].domain.subdomain_id == 0) {
			*ram_mbox_addr = ssow->vf[i].ram_mbox_addr;
			break;
		}
	}
	spin_unlock(&octeontx_ssow_devices_lock);

	if (i != ssow->total_vfs)
		return 0;
	else
		return -ENOENT;
}

static int ssow_pf_receive_message(u32 id, u16 domain_id,
				   struct mbox_hdr *hdr,
					union mbox_data *req,
					union mbox_data *resp)
{
	struct ssowpf *ssow = NULL;
	struct ssowpf *curr;
	int vf_idx = -1;
	int i;

	resp->data = 0;
	spin_lock(&octeontx_ssow_devices_lock);

	list_for_each_entry(curr, &octeontx_ssow_devices, list) {
		if (curr->id == id) {
			ssow = curr;
			break;
		}
	}
	if (!ssow) {
		hdr->res_code = MBOX_RET_INVALID;
		spin_unlock(&octeontx_ssow_devices_lock);
		return -ENODEV;
	}

	/* locate the SSO VF master of domain (vf_idx == 0) */
	for (i = 0; i < ssow->total_vfs; i++) {
		if (ssow->vf[i].domain.in_use &&
		    ssow->vf[i].domain.domain_id == domain_id &&
		    ssow->vf[i].domain.subdomain_id == 0) {
			vf_idx = i;
			break;
		}
	}

	if (vf_idx < 0) {
		hdr->res_code = MBOX_RET_INVALID;
		spin_unlock(&octeontx_ssow_devices_lock);
		return -ENODEV;
	}

	switch (hdr->msg) {
	case IDENTIFY:
		identify(&ssow->vf[i], domain_id, hdr->vfid);
		hdr->res_code = MBOX_RET_SUCCESS;
		break;
	default:
		hdr->res_code = MBOX_RET_INVALID;
		spin_unlock(&octeontx_ssow_devices_lock);
		return -1;
	}
	spin_unlock(&octeontx_ssow_devices_lock);
	return 0;
}

static int ssow_pf_get_vf_count(u32 id)
{
	struct ssowpf *ssow = NULL;
	struct ssowpf *curr;

	spin_lock(&octeontx_ssow_devices_lock);

	list_for_each_entry(curr, &octeontx_ssow_devices, list) {
		if (curr->id == id) {
			ssow = curr;
			break;
		}
	}
	if (!ssow) {
		spin_unlock(&octeontx_ssow_devices_lock);
		return 0;
	}

	spin_unlock(&octeontx_ssow_devices_lock);
	return ssow->total_vfs;
}

void ssow_clear_nosched(u32 id, struct ssowpf_vf *vf, u64 grp_mask)
{
	u64 reg;
	u64 grp;
	int j;
	int ret;

	for (j = 0; j < SSO_IENT_MAX; j++) {
		ret = sso_pf_get_value(id, SSO_PF_IENTX_GRP(j), &reg);
		if (ret)
			return;

		grp = ((reg >> SSO_IENT_GRP_GRP_SHIFT) & SSO_IENT_GRP_GRP_MASK);
		if (((grp_mask >> grp) & 0x1) == 0x1) {
			reg = sso_pf_get_value(id, SSO_PF_IENTX_WQP(j), &reg);
			if (reg != 0)
				writeq_relaxed(reg, vf->domain.reg_base +
						SSOW_VF_VHWSX_OP_CLR_NSCHED(0));
		}
	}
}

static void ssow_vf_get_work(u64 addr, struct wqe_s *wqe)
{
	u64 work0 = 0;
	u64 work1 = 0;

	asm volatile("ldp %0, %1, [%2]\n\t" : "=r" (work0), "=r" (work1)
			: "r" (addr));

	wqe->work0 = work0;
	if (work1)
		wqe->work1 = phys_to_virt(work1);
	else
		wqe->work1 = NULL;
}

static int __get_sso_group_pend(u32 id, u64 grp_mask)
{
	int cq_cnt;
	int ds_cnt;
	int aq_cnt;
	int count = 0;
	int i;
	u64 reg;

	for_each_set_bit(i, (const unsigned long *)&grp_mask,
			 sizeof(grp_mask)) {
		aq_cnt = 0;
		cq_cnt = 0;
		ds_cnt = 0;
		sso_vf_get_value(id, i, SSO_VF_VHGRPX_AQ_CNT(0), &reg);
		aq_cnt = reg & (0xffffffff);
		sso_vf_get_value(id, i, SSO_VF_VHGRPX_INT_CNT(0), &reg);
		ds_cnt = (reg >> 16) & 0x1fff;
		cq_cnt = (reg >> 32) & 0x1fff;
		count +=  cq_cnt + aq_cnt + ds_cnt;
	}
	return count;
}

int ssow_reset_domain(u32 id, u16 domain_id, u64 grp_mask)
{
	struct ssowpf *ssow = NULL;
	struct ssowpf *curr;
	int i, ret = 0;
	bool de_sched = false;
	int retry = 0;
	u64 addr;
	int count = 0;
	struct wqe_s wqe;
	u64 reg;
	void __iomem *reg_base;

	spin_lock(&octeontx_ssow_devices_lock);
	list_for_each_entry(curr, &octeontx_ssow_devices, list) {
		if (curr->id == id) {
			ssow = curr;
			break;
		}
	}

	if (!ssow) {
		ret = -EINVAL;
		goto unlock;
	}

	/* 0. Clear any active TAG switches
	 * 1. Loop thorugh SSO_ENT_GRP and clear NSCHED
	 * 2. do get_work on all HWS until VHGRP_INT_CNT and AQ_CNT == 0
	 */

	for (i = 0; i < ssow->total_vfs; i++) {
		if (ssow->vf[i].domain.in_use &&
		    ssow->vf[i].domain.domain_id == domain_id) {
			sso_pf_set_value(id, SSO_PF_HWSX_SX_GRPMASK(i, 0),
					 grp_mask);
			sso_pf_set_value(id, SSO_PF_HWSX_SX_GRPMASK(i, 1),
					 grp_mask);

			reg_base = ssow->vf[i].domain.reg_base;
			reg = readq_relaxed(reg_base +
					    SSOW_VF_VHWSX_PENDTAG(0));
			if (reg >> 63) {
				if (((reg >> 32) & 0x3) < 2)
					writeq_relaxed(0x0, reg_base +
						SSOW_VF_VHWSX_OP_DESCHED(0));
			} else {
				reg = readq_relaxed(reg_base +
						    SSOW_VF_VHWSX_WQP(0));
				if (reg) {
					reg = readq_relaxed(reg_base +
							SSOW_VF_VHWSX_TAG(0));

					if (((reg >> 32) & 0x3) < 2)
						writeq_relaxed(0x0, reg_base +
					      SSOW_VF_VHWSX_OP_SWTAG_UNTAG(0));
				}
			}

			if (!de_sched) {
				ssow_clear_nosched(id, &ssow->vf[i], grp_mask);
				de_sched = true;
			}

			addr = ((u64)ssow->vf[i].domain.reg_base +
					SSOW_VF_VHWSX_OP_GET_WORK0(0));
			retry = 0;
			do {
				wqe.work0 = 0;
				wqe.work1 = 0;
				ssow_vf_get_work(addr, &wqe);
				if (wqe.work1 == 0)
					retry++;
				count = __get_sso_group_pend(id, grp_mask);
			} while (count && retry < 1000);
			sso_pf_set_value(id, SSO_PF_HWSX_SX_GRPMASK(i, 0), 0);
			sso_pf_set_value(id, SSO_PF_HWSX_SX_GRPMASK(i, 1), 0);
		}
	}
	if (count)
		dev_err(&ssow->pdev->dev, "Failed to reset vf[%d]\n", i);

	for (i = 0; i < ssow->total_vfs; i++) {
		if (ssow->vf[i].domain.in_use &&
		    ssow->vf[i].domain.domain_id == domain_id) {
			identify(&ssow->vf[i], domain_id,
				 ssow->vf[i].domain.subdomain_id);
		}
	}

unlock:
	spin_unlock(&octeontx_ssow_devices_lock);
	return ret;
}

struct ssowpf_com_s ssowpf_com = {
	.create_domain = ssow_pf_create_domain,
	.destroy_domain = ssow_pf_destroy_domain,
	.reset_domain = ssow_reset_domain,
	.receive_message = ssow_pf_receive_message,
	.get_vf_count = ssow_pf_get_vf_count,
	.get_ram_mbox_addr = ssow_pf_get_ram_mbox_addr
};
EXPORT_SYMBOL(ssowpf_com);

static int ssow_sriov_configure(struct pci_dev *pdev, int numvfs)
{
	struct ssowpf *ssow = pci_get_drvdata(pdev);
	int ret = -EBUSY;
	int disable = 0;

	if (ssow->vfs_in_use != 0)
		return ret;

	ret = 0;
	if (ssow->flags & SSOW_SRIOV_ENABLED)
		disable = 1;

	if (disable) {
		pci_disable_sriov(pdev);
		ssow->flags &= ~SSOW_SRIOV_ENABLED;
		ssow->total_vfs = 0;
	}

	if (numvfs > 0) {
		ret = pci_enable_sriov(pdev, numvfs);
		if (ret == 0) {
			ssow->flags |= SSOW_SRIOV_ENABLED;
			ssow->total_vfs = numvfs;
			ret = numvfs;
		}
	}
	return ret;
}

static int ssow_init(struct ssowpf *ssow)
{
	u64 sso_reg;
	u16 nr_hws;
	int i, ret;

	ret = 0;

	for (i = 0; i < SSOW_MAX_VF; i++) {
		ssow->vf[i].domain.in_use = 0;
		ssow->vf[i].domain.master = NULL;
		ssow->vf[i].domain.master_data = NULL;
	}

	/* assuming for 83xx node = 0 */
	ret = sso_pf_get_value(0, SSO_PF_CONST, &sso_reg);
	if (ret < 0) {
		dev_err(&ssow->pdev->dev, "Failed to read nw_hws from SSO_PF_CONST\n");
		return -1;
	}

	nr_hws = (sso_reg >> SSO_CONST_HWS_SHIFT) & SSO_CONST_HWS_MASK;

	return 0;
}

static int ssow_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct device *dev = &pdev->dev;
	struct ssowpf *ssow;
	int err = -ENOMEM;

	ssow = devm_kzalloc(dev, sizeof(*ssow), GFP_KERNEL);
	if (!ssow)
		return err;

	pci_set_drvdata(pdev, ssow);
	ssow->pdev = pdev;

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(dev, "Failed to enable PCI device\n");
		return err;
	}

	err = pci_request_regions(pdev, DRV_NAME);
	if (err) {
		dev_err(dev, "PCI request regions failed\n");
		return err;
	}

	/* set SSOW ID */
	ssow->id = atomic_add_return(1, &ssow_count);
	ssow->id -= 1;

	err = ssow_init(ssow);
	if (err < 0) {
		atomic_sub_return(1, &ssow_count);
		dev_err(dev, "failed to ssow_init\n");
		err = -EPROBE_DEFER;
		return err;
	}

	INIT_LIST_HEAD(&ssow->list);
	spin_lock(&octeontx_ssow_devices_lock);
	list_add(&ssow->list, &octeontx_ssow_devices);
	spin_unlock(&octeontx_ssow_devices_lock);
	return 0;
}

static void ssow_remove(struct pci_dev *pdev)
{
	ssow_sriov_configure(pdev, 0);
}

/* devices supported */
static const struct pci_device_id ssow_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, PCI_DEVICE_ID_OCTEONTX_SSOW_PF) },
	{ 0, } /* end of table */
};

static struct pci_driver ssow_driver = {
	.name = DRV_NAME,
	.id_table = ssow_id_table,
	.probe = ssow_probe,
	.remove = ssow_remove,
	.sriov_configure = ssow_sriov_configure,
};

MODULE_AUTHOR("Tirumalesh Chalamarla");
MODULE_DESCRIPTION("Cavium OCTEONTX SSOW Physical Function Driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRV_VERSION);
MODULE_DEVICE_TABLE(pci, ssow_id_table);

static int __init ssow_init_module(void)
{
	pr_info("%s, ver %s\n", DRV_NAME, DRV_VERSION);

	return pci_register_driver(&ssow_driver);
}

static void __exit ssow_cleanup_module(void)
{
	pci_unregister_driver(&ssow_driver);
}

module_init(ssow_init_module);
module_exit(ssow_cleanup_module);

// SPDX-License-Identifier: GPL-2.0
/* Marvell OcteonTx2 RVU Physcial Function ethernet driver
 *
 * Copyright (C) 2018 Marvell International Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/etherdevice.h>
#include <linux/of.h>
#include <linux/if_vlan.h>
#include <net/ip.h>
#include <linux/iommu.h>

#include "otx2_reg.h"
#include "otx2_common.h"
#include "otx2_txrx.h"
#include "otx2_struct.h"
#include "otx2_ptp.h"

#define DRV_NAME	"octeontx2-nicpf"
#define DRV_STRING	"Marvell OcteonTX2 NIC Physical Function Driver"
#define DRV_VERSION	"1.0"

#define MAX_ETHTOOL_FLOWS	36

/* Supported devices */
static const struct pci_device_id otx2_pf_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, PCI_DEVID_OCTEONTX2_RVU_PF) },
	{ 0, }  /* end of table */
};

MODULE_AUTHOR("Marvell International Ltd.");
MODULE_DESCRIPTION(DRV_STRING);
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRV_VERSION);
MODULE_DEVICE_TABLE(pci, otx2_pf_id_table);

enum {
	TYPE_PFAF,
	TYPE_PFVF,
};

static int otx2_config_hw_tx_tstamp(struct otx2_nic *pfvf, bool enable);
static int otx2_config_hw_rx_tstamp(struct otx2_nic *pfvf, bool enable);

static void otx2_disable_flr_me_intr(struct otx2_nic *pf)
{
	int irq;
	int vfs = pf->total_vfs;

	/* Disable VFs ME interrupts */
	otx2_write64(pf, RVU_PF_VFME_INT_ENA_W1CX(0), INTR_MASK(vfs));
	irq = pci_irq_vector(pf->pdev, RVU_PF_INT_VEC_VFME0);
	free_irq(irq, pf);

	/* Disable VFs FLR interrupts */
	otx2_write64(pf, RVU_PF_VFFLR_INT_ENA_W1CX(0), INTR_MASK(vfs));
	irq = pci_irq_vector(pf->pdev, RVU_PF_INT_VEC_VFFLR0);
	free_irq(irq, pf);

	if (vfs <= 64)
		return;

	otx2_write64(pf, RVU_PF_VFME_INT_ENA_W1CX(1), INTR_MASK(vfs - 64));
	irq = pci_irq_vector(pf->pdev, RVU_PF_INT_VEC_VFME1);
	free_irq(irq, pf);

	otx2_write64(pf, RVU_PF_VFFLR_INT_ENA_W1CX(1), INTR_MASK(vfs - 64));
	irq = pci_irq_vector(pf->pdev, RVU_PF_INT_VEC_VFFLR1);
	free_irq(irq, pf);
}

static void otx2_flr_wq_destroy(struct otx2_nic *pf)
{
	if (!pf->flr_wq)
		return;
	flush_workqueue(pf->flr_wq);
	destroy_workqueue(pf->flr_wq);
	pf->flr_wq = NULL;
}

static void otx2_flr_handler(struct work_struct *work)
{
	struct flr_work *flrwork = container_of(work, struct flr_work, work);
	struct otx2_nic *pf = flrwork->pf;
	int vf, reg = 0;
	struct msg_req *req;
	struct msg_rsp *rsp;

	vf = flrwork - pf->flr_wrk;

	/* Delete ethtool ntuple filters for the VF before FLR */
	if (otx2_delete_ethtool_flows_for_vf(pf, vf + 1))
		dev_err(pf->dev, "Unable to delete VF%d ntuple filters\n",
			vf);

	otx2_mbox_lock(&pf->mbox);
	req = otx2_mbox_alloc_msg_vf_flr(&pf->mbox);
	if (!req) {
		otx2_mbox_unlock(&pf->mbox);
		return;
	}
	req->hdr.pcifunc &= RVU_PFVF_FUNC_MASK;
	req->hdr.pcifunc |= (vf + 1) & RVU_PFVF_FUNC_MASK;

	if (!otx2_sync_mbox_msg(&pf->mbox)) {
		if (vf > 64) {
			reg = 1;
			vf = vf - 64;
		}
		rsp = (struct  msg_rsp *)
		      otx2_mbox_get_rsp(&pf->mbox.mbox, 0, &req->hdr);
		otx2_mbox_unlock(&pf->mbox);
		if (rsp->hdr.rc)
			return;
		/* clear transcation pending bit */
		otx2_write64(pf, RVU_PF_VFTRPENDX(reg), BIT_ULL(vf));
		otx2_write64(pf, RVU_PF_VFFLR_INT_ENA_W1SX(reg), BIT_ULL(vf));
	}

	otx2_mbox_unlock(&pf->mbox);
}

static irqreturn_t otx2_pf_flr_intr_handler(int irq, void *pf_irq)
{
	struct otx2_nic *pf = (struct otx2_nic *)pf_irq;
	int reg, dev, vf, start_vf, num_reg = 1;
	u64 intr;

	if (pf->total_vfs > 64)
		num_reg = 2;

	for (reg = 0; reg < num_reg; reg++) {
		intr = otx2_read64(pf, RVU_PF_VFFLR_INTX(reg));
		if (!intr)
			continue;
		start_vf =  64 * reg;
		for (vf = 0; vf < 64; vf++) {
			if (!(intr & BIT_ULL(vf)))
				continue;
			dev = vf + start_vf;
			queue_work(pf->flr_wq, &pf->flr_wrk[dev].work);
			/* Clear interrupt */
			otx2_write64(pf, RVU_PF_VFFLR_INTX(reg), BIT_ULL(vf));
			/* Disable the interrupt */
			otx2_write64(pf, RVU_PF_VFFLR_INT_ENA_W1CX(reg),
				     BIT_ULL(vf));
		}
	}
	return IRQ_HANDLED;
}

static irqreturn_t otx2_pf_me_intr_handler(int irq, void *pf_irq)
{
	struct otx2_nic *pf = (struct otx2_nic *)pf_irq;
	int vf, reg, num_reg = 1;
	u64 intr;

	if (pf->total_vfs > 64)
		num_reg = 2;

	for (reg = 0; reg < num_reg; reg++) {
		intr = otx2_read64(pf, RVU_PF_VFME_INTX(reg));
		if (!intr)
			continue;
		for (vf = 0; vf < 64; vf++) {
			if (!(intr & BIT_ULL(vf)))
				continue;
			/* clear trpend bit */
			otx2_write64(pf, RVU_PF_VFTRPENDX(reg), BIT_ULL(vf));
			/* clear interrupt */
			otx2_write64(pf, RVU_PF_VFME_INTX(reg), BIT_ULL(vf));
		}
	}
	return IRQ_HANDLED;
}

static int otx2_register_flr_me_intr(struct otx2_nic *pf)
{
	struct otx2_hw *hw = &pf->hw;
	int vfs = pf->total_vfs;
	char *irq_name;
	int ret;

	/* Register ME interrupt handler*/
	irq_name = &hw->irq_name[RVU_PF_INT_VEC_VFME0 * NAME_SIZE];
	snprintf(irq_name, NAME_SIZE, "RVUPF%d_ME0", rvu_get_pf(pf->pcifunc));
	ret = request_irq(pci_irq_vector(pf->pdev, RVU_PF_INT_VEC_VFME0),
			  otx2_pf_me_intr_handler, 0, irq_name, pf);
	if (ret) {
		dev_err(pf->dev,
			"RVUPF: IRQ registration failed for ME\n");
	}

	/* Register FLR interrupt handler */
	irq_name = &hw->irq_name[RVU_PF_INT_VEC_VFFLR0 * NAME_SIZE];
	snprintf(irq_name, NAME_SIZE, "RVUPF%d_FLR0", rvu_get_pf(pf->pcifunc));
	ret = request_irq(pci_irq_vector(pf->pdev, RVU_PF_INT_VEC_VFFLR0),
			  otx2_pf_flr_intr_handler, 0, irq_name, pf);
	if (ret) {
		dev_err(pf->dev,
			"RVUPF: IRQ registration failed for FLR\n");
		return ret;
	}

	if (pf->total_vfs > 64) {
		irq_name = &hw->irq_name[RVU_PF_INT_VEC_VFME1 * NAME_SIZE];
		snprintf(irq_name, NAME_SIZE, "RVUPF%d_ME1",
			 rvu_get_pf(pf->pcifunc));
		ret = request_irq(pci_irq_vector
				  (pf->pdev, RVU_PF_INT_VEC_VFME1),
				  otx2_pf_me_intr_handler, 0, irq_name, pf);
		if (ret) {
			dev_err(pf->dev,
				"RVUPF: IRQ registration failed for ME1\n");
		}
		irq_name = &hw->irq_name[RVU_PF_INT_VEC_VFFLR1 * NAME_SIZE];
		snprintf(irq_name, NAME_SIZE, "RVUPF%d_FLR1",
			 rvu_get_pf(pf->pcifunc));
		ret = request_irq(pci_irq_vector
				  (pf->pdev, RVU_PF_INT_VEC_VFFLR1),
				  otx2_pf_flr_intr_handler, 0, irq_name, pf);
		if (ret) {
			dev_err(pf->dev,
				"RVUPF: IRQ registration failed for FLR1\n");
			return ret;
		}
	}

	/* Enable ME interrupt for all VFs*/
	otx2_write64(pf, RVU_PF_VFME_INTX(0), INTR_MASK(vfs));
	otx2_write64(pf, RVU_PF_VFME_INT_ENA_W1SX(0), INTR_MASK(vfs));

	/* Enable FLR interrupt for all VFs*/
	otx2_write64(pf, RVU_PF_VFFLR_INTX(0), INTR_MASK(vfs));
	otx2_write64(pf, RVU_PF_VFFLR_INT_ENA_W1SX(0), INTR_MASK(vfs));

	if (pf->total_vfs > 64) {
		vfs = pf->total_vfs - 64 - 1;

		otx2_write64(pf, RVU_PF_VFME_INTX(1), INTR_MASK(vfs));
		otx2_write64(pf, RVU_PF_VFME_INT_ENA_W1SX(1), INTR_MASK(vfs));

		otx2_write64(pf, RVU_PF_VFFLR_INTX(1), INTR_MASK(vfs));
		otx2_write64(pf, RVU_PF_VFFLR_INT_ENA_W1SX(1), INTR_MASK(vfs));
	}
	return 0;
}

static int otx2_pf_flr_init(struct otx2_nic *pf, int num_vfs)
{
	int vf;

	pf->flr_wq = alloc_workqueue("otx2_pf_flr_wq", WQ_UNBOUND | WQ_HIGHPRI
				     | WQ_MEM_RECLAIM, 1);
	if (!pf->flr_wq)
		return -ENOMEM;

	pf->flr_wrk = devm_kcalloc(pf->dev, num_vfs,
				   sizeof(struct flr_work), GFP_KERNEL);
	if (!pf->flr_wrk) {
		destroy_workqueue(pf->flr_wq);
		return -ENOMEM;
	}

	for (vf = 0; vf < num_vfs; vf++) {
		pf->flr_wrk[vf].pf = pf;
		INIT_WORK(&pf->flr_wrk[vf].work, otx2_flr_handler);
	}

	return 0;
}

static void otx2_queue_work(struct mbox *mw, struct workqueue_struct *mbox_wq,
			    int first, int mdevs, u64 intr, int type)
{
	struct otx2_mbox_dev *mdev;
	struct otx2_mbox *mbox;
	struct mbox_hdr *hdr;
	int i;

	for (i = first; i < mdevs; i++) {
		/* start from 0 */
		if (!(intr & BIT_ULL(i - first)))
			continue;
		mbox = &mw->mbox;
		mdev = &mbox->dev[i];
		if (type == TYPE_PFAF)
			otx2_sync_mbox_bbuf(mbox, i);
		hdr = mdev->mbase + mbox->rx_start;
		/*The hdr->num_msgs is set to zero immediately in the interrupt
		 * handler to  ensure that it holds a correct value next time
		 * when the interrupt handler is called.
		 * pf->mbox.num_msgs holds the data for use in pfaf_mbox_handler
		 * pf>mbox.up_num_msgs holds the data for use in
		 * pfaf_mbox_up_handler.
		 */
		if (hdr->num_msgs) {
			mw->num_msgs = hdr->num_msgs;
			hdr->num_msgs = 0;
			if (type == TYPE_PFAF)
				memset(mbox->hwbase + mbox->rx_start, 0,
				       ALIGN(sizeof(struct mbox_hdr),
					     sizeof(u64)));

			queue_work(mbox_wq, &mw[i].mbox_wrk);
		}
		mbox = &mw->mbox_up;
		mdev = &mbox->dev[i];
		if (type == TYPE_PFAF)
			otx2_sync_mbox_bbuf(mbox, i);
		hdr = mdev->mbase + mbox->rx_start;
		if (hdr->num_msgs) {
			mw->up_num_msgs = hdr->num_msgs;
			hdr->num_msgs = 0;
			if (type == TYPE_PFAF)
				memset(mbox->hwbase + mbox->rx_start, 0,
				       ALIGN(sizeof(struct mbox_hdr),
					     sizeof(u64)));

			queue_work(mbox_wq, &mw[i].mbox_up_wrk);
		}

	}
}

static void otx2_forward_msg_pfvf(struct otx2_mbox_dev *mdev,
				  struct otx2_mbox *pfvf_mbox, void *bbuf_base,
				  int devid)
{
	struct otx2_mbox_dev *src_mdev = mdev;
	int offset;

	/* Msgs are already copied, trigger VF's mbox irq */
	smp_wmb();

	offset = pfvf_mbox->trigger | (devid << pfvf_mbox->tr_shift);
	writeq(1, (void __iomem *)pfvf_mbox->reg_base + offset);

	/* Restore VF's mbox bounce buffer region address */
	src_mdev->mbase = bbuf_base;
}

static int otx2_forward_vf_mbox_msgs(struct otx2_nic *pf,
				     struct otx2_mbox *src_mbox,
				     int dir, int vf, int num_msgs)
{
	struct otx2_mbox_dev *src_mdev, *dst_mdev;
	struct mbox_hdr *mbox_hdr;
	struct mbox_hdr *req_hdr;
	struct mbox *dst_mbox;
	int dst_size, err;

	if (dir == MBOX_DIR_PFAF) {
		/* Set VF's mailbox memory as PF's bounce buffer memory, so
		 * that explicit copying of VF's msgs to PF=>AF mbox region
		 * and AF=>PF responses to VF's mbox region can be avoided.
		 */
		src_mdev = &src_mbox->dev[vf];
		mbox_hdr = src_mbox->hwbase +
				src_mbox->rx_start + (vf * MBOX_SIZE);
		req_hdr = (struct mbox_hdr *)(src_mdev->mbase +
					      src_mbox->rx_start);
		req_hdr->num_msgs = num_msgs;
		dst_mbox = &pf->mbox;
		dst_size = dst_mbox->mbox.tx_size -
				ALIGN(sizeof(*mbox_hdr), MBOX_MSG_ALIGN);
		/* Check if msgs fit into destination area */
		if (mbox_hdr->msg_size > dst_size)
			return -EINVAL;

		dst_mdev = &dst_mbox->mbox.dev[0];

		otx2_mbox_lock(&pf->mbox);
		dst_mdev->mbase = src_mdev->mbase;
		dst_mdev->msg_size = mbox_hdr->msg_size;
		dst_mdev->num_msgs = mbox_hdr->num_msgs;
		err = otx2_sync_mbox_msg(dst_mbox);
		if (err) {
			dev_warn(pf->dev,
				 "AF not responding to VF%d messages\n", vf);
			/* restore PF mbase and exit */
			dst_mdev->mbase = pf->mbox.bbuf_base;
			otx2_mbox_unlock(&pf->mbox);
			return err;
		}
		/* At this point, all the VF messages sent to AF are acked
		 * with proper responses and responses are copied to VF
		 * mailbox hence raise interrupt to VF.
		 */
		req_hdr = (struct mbox_hdr *)(dst_mdev->mbase +
					      dst_mbox->mbox.rx_start);
		req_hdr->num_msgs = num_msgs;

		otx2_forward_msg_pfvf(dst_mdev, &pf->mbox_pfvf[0].mbox,
				      pf->mbox.bbuf_base, vf);
		otx2_mbox_unlock(&pf->mbox);
	} else if (dir == MBOX_DIR_PFVF_UP) {
		src_mdev = &src_mbox->dev[0];
		mbox_hdr = src_mbox->hwbase + src_mbox->rx_start;
		req_hdr = (struct mbox_hdr *)(src_mdev->mbase +
					      src_mbox->rx_start);
		req_hdr->num_msgs = num_msgs;

		dst_mbox = &pf->mbox_pfvf[0];
		dst_size = dst_mbox->mbox_up.tx_size -
				ALIGN(sizeof(*mbox_hdr), MBOX_MSG_ALIGN);
		/* Check if msgs fit into destination area */
		if (mbox_hdr->msg_size > dst_size)
			return -EINVAL;

		dst_mdev = &dst_mbox->mbox_up.dev[vf];
		dst_mdev->mbase = src_mdev->mbase;
		dst_mdev->msg_size = mbox_hdr->msg_size;
		dst_mdev->num_msgs = mbox_hdr->num_msgs;
		err = otx2_sync_mbox_up_msg(dst_mbox, vf);
		if (err) {
			dev_warn(pf->dev,
				 "VF%d is not responding to mailbox\n", vf);
			return err;
		}
	} else if (dir == MBOX_DIR_VFPF_UP) {
		req_hdr = (struct mbox_hdr *)(src_mbox->dev[0].mbase +
					      src_mbox->rx_start);
		req_hdr->num_msgs = num_msgs;
		otx2_forward_msg_pfvf(&pf->mbox_pfvf->mbox_up.dev[vf],
				      &pf->mbox.mbox_up,
				      pf->mbox_pfvf[vf].bbuf_base,
				      0);
	}

	return 0;
}

static void otx2_pfvf_mbox_handler(struct work_struct *work)
{
	struct mbox_msghdr *msg = NULL;
	int offset, vf_idx, id, err;
	struct otx2_mbox_dev *mdev;
	struct mbox_hdr *req_hdr;
	struct otx2_mbox *mbox;
	struct mbox *vf_mbox;
	struct otx2_nic *pf;

	vf_mbox = container_of(work, struct mbox, mbox_wrk);
	pf = vf_mbox->pfvf;
	vf_idx = vf_mbox - pf->mbox_pfvf;
	vf_mbox = &pf->mbox_pfvf[0];
	mbox = &vf_mbox->mbox;
	mdev = &mbox->dev[vf_idx];
	req_hdr = (struct mbox_hdr *)(mdev->mbase + mbox->rx_start);
	if (vf_mbox->num_msgs == 0)
		return;

	offset = ALIGN(sizeof(*req_hdr), MBOX_MSG_ALIGN);

	for (id = 0; id < vf_mbox->num_msgs; id++) {
		msg = (struct mbox_msghdr *)(mdev->mbase + mbox->rx_start +
					     offset);

		if (msg->sig != OTX2_MBOX_REQ_SIG)
			goto inval_msg;

		/* Set VF's number in each of the msg */
		msg->pcifunc &= RVU_PFVF_FUNC_MASK;
		msg->pcifunc |= (vf_idx + 1) & RVU_PFVF_FUNC_MASK;
		offset = msg->next_msgoff;
	}
	err = otx2_forward_vf_mbox_msgs(pf, mbox, MBOX_DIR_PFAF, vf_idx,
					vf_mbox->num_msgs);
	/* mbox messages in the same direction to be handled by same
	 * mailbox occurs serially. So write to vf_mbox->num_msgs
	 * happens only after the previous context is done with it.
	 */
	vf_mbox->num_msgs = 0;
	if (err)
		goto inval_msg;
	return;

inval_msg:
	otx2_reply_invalid_msg(mbox, vf_idx, 0, msg->id);
	otx2_mbox_msg_send(mbox, vf_idx);
}

static void otx2_pfvf_mbox_up_handler(struct work_struct *work)
{
	struct mbox *vf_mbox = container_of(work, struct mbox, mbox_up_wrk);
	struct otx2_nic *pf = vf_mbox->pfvf;
	struct otx2_mbox_dev *mdev;
	int offset, id, vf_idx = 0;
	struct mbox_hdr *rsp_hdr;
	struct mbox_msghdr *msg;
	struct otx2_mbox *mbox;

	vf_idx = vf_mbox - pf->mbox_pfvf;
	vf_mbox = &pf->mbox_pfvf[0];
	mbox = &vf_mbox->mbox_up;
	mdev = &mbox->dev[vf_idx];

	if (vf_mbox->up_num_msgs == 0)
		return;

	rsp_hdr = (struct mbox_hdr *)(mdev->mbase + mbox->rx_start);
	offset = mbox->rx_start + ALIGN(sizeof(*rsp_hdr), MBOX_MSG_ALIGN);

	for (id = 0; id < vf_mbox->up_num_msgs; id++) {
		msg = mdev->mbase + offset;

		if (msg->id >= MBOX_MSG_MAX) {
			dev_err(pf->dev,
				"Mbox msg with unknown ID 0x%x\n", msg->id);
			goto end;
		}

		if (msg->sig != OTX2_MBOX_RSP_SIG) {
			dev_err(pf->dev,
				"Mbox msg with wrong signature %x, ID 0x%x\n",
				msg->sig, msg->id);
			goto end;
		}

		switch (msg->id) {
		case MBOX_MSG_CGX_LINK_EVENT:
			break;
		default:
			if (msg->rc)
				dev_err(pf->dev,
					"Mbox msg response has err %d, ID 0x%x\n",
					msg->rc, msg->id);
			break;
		}

end:
		offset = mbox->rx_start + msg->next_msgoff;
		mdev->msgs_acked++;
	}
	/* mbox messages in the same direction to be handled by same
	 * mailbox occurs serially. So write to vf_mbox->up_num_msgs
	 * happens only after the previous context is done with it.
	 */
	vf_mbox->up_num_msgs = 0;

	otx2_mbox_reset(mbox, vf_idx);
}

static irqreturn_t otx2_pfvf_mbox_intr_handler(int irq, void *pf_irq)
{
	struct otx2_nic *pf = (struct otx2_nic *)(pf_irq);
	int vfs = pf->total_vfs;
	struct mbox *mbox;
	u64 intr;

	mbox = pf->mbox_pfvf;
	/* Handle VF interrupts */
	if (vfs > 64) {
		intr = otx2_read64(pf, RVU_PF_VFPF_MBOX_INTX(1));
		otx2_write64(pf, RVU_PF_VFPF_MBOX_INTX(1), intr);
		otx2_queue_work(mbox, pf->mbox_pfvf_wq, 64, vfs, intr,
				TYPE_PFVF);
		vfs -= 64;
	}

	intr = otx2_read64(pf, RVU_PF_VFPF_MBOX_INTX(0));
	otx2_write64(pf, RVU_PF_VFPF_MBOX_INTX(0), intr);

	otx2_queue_work(mbox, pf->mbox_pfvf_wq, 0, vfs, intr, TYPE_PFVF);

	return IRQ_HANDLED;
}

static int otx2_pfvf_mbox_init(struct otx2_nic *pf, int numvfs)
{
	void __iomem *hwbase;
	struct mbox *mbox;
	int err, vf;
	u64 base;

	if (!numvfs)
		return -EINVAL;

	pf->mbox_pfvf = devm_kcalloc(&pf->pdev->dev, numvfs,
				     sizeof(struct mbox), GFP_KERNEL);
	if (!pf->mbox_pfvf)
		return -ENOMEM;

	pf->mbox_pfvf_wq = alloc_workqueue("otx2_pfvf_mailbox",
					   WQ_UNBOUND | WQ_HIGHPRI |
					   WQ_MEM_RECLAIM, 1);
	if (!pf->mbox_pfvf_wq)
		return -ENOMEM;

	base = readq((void __iomem *)((u64)pf->reg_base + RVU_PF_VF_BAR4_ADDR));
	hwbase = ioremap_wc(base, MBOX_SIZE * pf->total_vfs);

	if (!hwbase) {
		err = -ENOMEM;
		goto free_wq;
	}

	mbox = &pf->mbox_pfvf[0];
	err = otx2_mbox_init(&mbox->mbox, hwbase, pf->pdev, pf->reg_base,
			     MBOX_DIR_PFVF, numvfs);
	if (err)
		goto free_iomem;

	err = otx2_mbox_init(&mbox->mbox_up, hwbase, pf->pdev, pf->reg_base,
			     MBOX_DIR_PFVF_UP, numvfs);
	if (err)
		goto free_iomem;

	for (vf = 0; vf < numvfs; vf++) {
		mbox->pfvf = pf;
		INIT_WORK(&mbox->mbox_wrk, otx2_pfvf_mbox_handler);
		INIT_WORK(&mbox->mbox_up_wrk, otx2_pfvf_mbox_up_handler);
		mbox++;
	}

	return 0;

free_iomem:
	if (hwbase)
		iounmap(hwbase);
free_wq:
	destroy_workqueue(pf->mbox_pfvf_wq);
	return err;
}

static void otx2_pfvf_mbox_destroy(struct otx2_nic *pf)
{
	struct mbox *mbox = &pf->mbox_pfvf[0];

	if (!mbox)
		return;

	if (pf->mbox_pfvf_wq) {
		flush_workqueue(pf->mbox_pfvf_wq);
		destroy_workqueue(pf->mbox_pfvf_wq);
		pf->mbox_pfvf_wq = NULL;
	}

	if (mbox->mbox.hwbase)
		iounmap(mbox->mbox.hwbase);

	otx2_mbox_destroy(&mbox->mbox);
}

static void otx2_enable_pfvf_mbox_intr(struct otx2_nic *pf)
{
	int bits;

	/* Clear PF <=> VF mailbox IRQ */
	otx2_write64(pf, RVU_PF_VFPF_MBOX_INTX(0), ~0ull);
	otx2_write64(pf, RVU_PF_VFPF_MBOX_INTX(1), ~0ull);

	/* Enable PF <=> VF mailbox IRQ */
	bits = ((pf->total_vfs - 1) % 64);
	otx2_write64(pf, RVU_PF_VFPF_MBOX_INT_ENA_W1SX(0),
		     GENMASK_ULL(bits, 0));

	if (pf->total_vfs > 64) {
		bits = pf->total_vfs - 64 - 1;
		otx2_write64(pf, RVU_PF_VFPF_MBOX_INT_ENA_W1SX(1),
			     GENMASK_ULL(bits, 0));
	}
}

static void otx2_disable_pfvf_mbox_intr(struct otx2_nic *pf)
{
	int vector;

	/* Disable PF <=> VF mailbox IRQ */
	otx2_write64(pf, RVU_PF_VFPF_MBOX_INT_ENA_W1CX(0), ~0ull);
	otx2_write64(pf, RVU_PF_VFPF_MBOX_INT_ENA_W1CX(1), ~0ull);

	otx2_write64(pf, RVU_PF_VFPF_MBOX_INTX(0), ~0ull);
	vector = pci_irq_vector(pf->pdev, RVU_PF_INT_VEC_VFPF_MBOX0);
	free_irq(vector, pf);

	if (pf->total_vfs > 64) {
		otx2_write64(pf, RVU_PF_VFPF_MBOX_INTX(1), ~0ull);
		vector = pci_irq_vector(pf->pdev, RVU_PF_INT_VEC_VFPF_MBOX1);
		free_irq(vector, pf);
	}
}

static int otx2_register_pfvf_mbox_intr(struct otx2_nic *pf)
{
	struct otx2_hw *hw = &pf->hw;
	char *irq_name;
	int err;

	/* Register MBOX0 interrupt handler */
	irq_name = &hw->irq_name[RVU_PF_INT_VEC_VFPF_MBOX0 * NAME_SIZE];
	if (pf->pcifunc)
		snprintf(irq_name, NAME_SIZE,
			 "RVUPF%d_VF Mbox0", rvu_get_pf(pf->pcifunc));
	else
		snprintf(irq_name, NAME_SIZE, "RVUPF_VF Mbox0");
	err = request_irq(pci_irq_vector(pf->pdev, RVU_PF_INT_VEC_VFPF_MBOX0),
			  otx2_pfvf_mbox_intr_handler, 0, irq_name, pf);
	if (err) {
		dev_err(pf->dev,
			"RVUPF: IRQ registration failed for PFAF mbox0 irq\n");
		return err;
	}

	if (pf->total_vfs > 64) {
		/* Register MBOX1 interrupt handler */
		irq_name = &hw->irq_name[RVU_PF_INT_VEC_VFPF_MBOX1 * NAME_SIZE];
		if (pf->pcifunc)
			snprintf(irq_name, NAME_SIZE,
				 "RVUPF%d_VF Mbox1", rvu_get_pf(pf->pcifunc));
		else
			snprintf(irq_name, NAME_SIZE, "RVUPF_VF Mbox1");
		err = request_irq(pci_irq_vector(pf->pdev,
						 RVU_PF_INT_VEC_VFPF_MBOX1),
						 otx2_pfvf_mbox_intr_handler,
						 0, irq_name, pf);
		if (err) {
			dev_err(pf->dev,
				"RVUPF: IRQ registration failed for PFAF mbox1 irq\n");
			return err;
		}
	}

	otx2_enable_pfvf_mbox_intr(pf);

	return 0;
}

static void otx2_process_pfaf_mbox_msg(struct otx2_nic *pf,
				       struct mbox_msghdr *msg)
{
	int devid;

	if (msg->id >= MBOX_MSG_MAX) {
		dev_err(pf->dev,
			"Mbox msg with unknown ID 0x%x\n", msg->id);
		return;
	}

	if (msg->sig != OTX2_MBOX_RSP_SIG) {
		dev_err(pf->dev,
			"Mbox msg with wrong signature %x, ID 0x%x\n",
			 msg->sig, msg->id);
		return;
	}

	/* message response heading VF */
	devid = msg->pcifunc & RVU_PFVF_FUNC_MASK;
	if (devid) {
		struct otx2_vf_config *config = &pf->vf_configs[devid - 1];
		struct delayed_work *dwork;

		switch (msg->id) {
		case MBOX_MSG_NIX_LF_START_RX:
			config->intf_down = false;
			dwork = &config->link_event_work;
			schedule_delayed_work(dwork, msecs_to_jiffies(100));
			dwork = &config->mac_vlan_work;
			schedule_delayed_work(dwork, msecs_to_jiffies(100));
			break;
		case MBOX_MSG_NIX_LF_STOP_RX:
			config->intf_down = true;
			break;
		}

		return;
	}

	switch (msg->id) {
	case MBOX_MSG_READY:
		pf->pcifunc = msg->pcifunc;
		break;
	case MBOX_MSG_MSIX_OFFSET:
		mbox_handler_msix_offset(pf, (struct msix_offset_rsp *)msg);
		break;
	case MBOX_MSG_NPA_LF_ALLOC:
		mbox_handler_npa_lf_alloc(pf, (struct npa_lf_alloc_rsp *)msg);
		break;
	case MBOX_MSG_NIX_LF_ALLOC:
		mbox_handler_nix_lf_alloc(pf, (struct nix_lf_alloc_rsp *)msg);
		break;
	case MBOX_MSG_NIX_TXSCH_ALLOC:
		mbox_handler_nix_txsch_alloc(pf,
					     (struct nix_txsch_alloc_rsp *)msg);
		break;
	case MBOX_MSG_NIX_BP_ENABLE:
		mbox_handler_nix_bp_enable(pf, (struct nix_bp_cfg_rsp *)msg);
		break;
	case MBOX_MSG_CGX_STATS:
		mbox_handler_cgx_stats(pf, (struct cgx_stats_rsp *)msg);
		break;
	default:
		if (msg->rc)
			dev_err(pf->dev,
				"Mbox msg response has err %d, ID 0x%x\n",
				msg->rc, msg->id);
		break;
	}
}

static void otx2_pfaf_mbox_handler(struct work_struct *work)
{
	struct otx2_mbox_dev *mdev;
	struct mbox_hdr *rsp_hdr;
	struct mbox_msghdr *msg;
	struct otx2_mbox *mbox;
	struct mbox *af_mbox;
	struct otx2_nic *pf;
	int offset, id;

	af_mbox = container_of(work, struct mbox, mbox_wrk);
	mbox = &af_mbox->mbox;
	mdev = &mbox->dev[0];
	rsp_hdr = (struct mbox_hdr *)(mdev->mbase + mbox->rx_start);
	if (af_mbox->num_msgs == 0)
		return;
	offset = mbox->rx_start + ALIGN(sizeof(*rsp_hdr), MBOX_MSG_ALIGN);
	pf = af_mbox->pfvf;

	for (id = 0; id < af_mbox->num_msgs; id++) {
		msg = (struct mbox_msghdr *)(mdev->mbase + offset);
		otx2_process_pfaf_mbox_msg(pf, msg);
		offset = mbox->rx_start + msg->next_msgoff;
		mdev->msgs_acked++;
	}

	otx2_mbox_reset(mbox, 0);

	/* mbox messages in the same direction to be handled by same
	 * mailbox occurs serially. So write to af_mbox->num_msgs
	 * happens only after the previous context is done with it.
	 */
	af_mbox->num_msgs = 0;
	/* Clear the IRQ */
	smp_wmb();
	otx2_write64(pf, RVU_PF_INT, BIT_ULL(0));
}

static void otx2_handle_link_event(struct otx2_nic *pf)
{
	struct cgx_link_user_info *linfo = &pf->linfo;
	struct net_device *netdev = pf->netdev;

	pr_info("%s NIC Link is %s %d Mbps %s duplex\n", netdev->name,
		linfo->link_up ? "UP" : "DOWN", linfo->speed,
		linfo->full_duplex ? "Full" : "Half");
	if (linfo->link_up) {
		netif_carrier_on(netdev);
		netif_tx_start_all_queues(netdev);
	} else {
		netif_tx_stop_all_queues(netdev);
		netif_carrier_off(netdev);
	}
}

int otx2_mbox_up_handler_cgx_link_event(struct otx2_nic *pf,
					struct cgx_link_info_msg *msg,
					struct msg_rsp *rsp)
{
	int i;

	pf->linfo = msg->link_info;

	/* notify VFs about link event */
	for (i = 0; i < pci_num_vf(pf->pdev); i++) {
		struct otx2_vf_config *config = &pf->vf_configs[i];
		struct delayed_work *dwork = &config->link_event_work;

		if (config->intf_down)
			continue;

		schedule_delayed_work(dwork, msecs_to_jiffies(100));
	}

	/* interface has not been fully configured yet */
	if (pf->intf_down)
		return 0;

	otx2_handle_link_event(pf);
	return 0;
}
EXPORT_SYMBOL(otx2_mbox_up_handler_cgx_link_event);

static int otx2_process_mbox_msg_up(struct otx2_nic *pf,
				    struct mbox_msghdr *req)
{
	/* Check if valid, if not reply with a invalid msg */
	if (req->sig != OTX2_MBOX_REQ_SIG) {
		otx2_reply_invalid_msg(&pf->mbox.mbox_up, 0, 0, req->id);
		return -ENODEV;
	}

	switch (req->id) {
#define M(_name, _id, _fn_name, _req_type, _rsp_type)			\
	case _id: {							\
		struct _rsp_type *rsp;					\
		int err;						\
									\
		rsp = (struct _rsp_type *)otx2_mbox_alloc_msg(		\
			&pf->mbox.mbox_up, 0,				\
			sizeof(struct _rsp_type));			\
		if (!rsp)						\
			return -ENOMEM;					\
									\
		rsp->hdr.id = _id;					\
		rsp->hdr.sig = OTX2_MBOX_RSP_SIG;			\
		rsp->hdr.pcifunc = 0;					\
		rsp->hdr.rc = 0;					\
									\
		err = otx2_mbox_up_handler_ ## _fn_name(		\
			pf, (struct _req_type *)req, rsp);		\
		return err;						\
	}
MBOX_UP_CGX_MESSAGES
#undef M
		break;
	default:
		otx2_reply_invalid_msg(&pf->mbox.mbox_up, 0, 0, req->id);
		return -ENODEV;
	}
	return 0;
}

static void otx2_pfaf_mbox_up_handler(struct work_struct *work)
{
	struct mbox *af_mbox = container_of(work, struct mbox, mbox_up_wrk);
	struct otx2_mbox *mbox = &af_mbox->mbox_up;
	struct otx2_mbox_dev *mdev = &mbox->dev[0];
	struct otx2_nic *pf = af_mbox->pfvf;
	int offset, id, devid = 0;
	struct mbox_hdr *rsp_hdr;
	struct mbox_msghdr *msg;

	rsp_hdr = (struct mbox_hdr *)(mdev->mbase + mbox->rx_start);
	if (af_mbox->up_num_msgs == 0)
		return;

	offset = mbox->rx_start + ALIGN(sizeof(*rsp_hdr), MBOX_MSG_ALIGN);

	for (id = 0; id < af_mbox->up_num_msgs; id++) {
		msg = (struct mbox_msghdr *)(mdev->mbase + offset);

		devid = msg->pcifunc & RVU_PFVF_FUNC_MASK;
		/* Skip processing VF's messages */
		if (!devid)
			otx2_process_mbox_msg_up(pf, msg);
		offset = mbox->rx_start + msg->next_msgoff;
	}
	if (devid) {
		otx2_forward_vf_mbox_msgs(pf, &pf->mbox.mbox_up,
					  MBOX_DIR_PFVF_UP, devid - 1,
					  af_mbox->up_num_msgs);
		return;
	}
	/* mbox messages in the same direction to be handled by same
	 * mailbox occurs serially. So write to af_mbox->up_num_msgs
	 * happens only after the previous context is done with it.
	 */
	af_mbox->up_num_msgs = 0;

	otx2_mbox_msg_send(mbox, 0);
}

static irqreturn_t otx2_pfaf_mbox_intr_handler(int irq, void *pf_irq)
{
	struct otx2_nic *pf = (struct otx2_nic *)pf_irq;
	struct mbox *mbox;

	mbox = &pf->mbox;
	otx2_queue_work(mbox, pf->mbox_wq, 0, 1, 1, TYPE_PFAF);

	/* Clear the IRQ */
	otx2_write64(pf, RVU_PF_INT, BIT_ULL(0));

	return IRQ_HANDLED;
}

static void otx2_disable_mbox_intr(struct otx2_nic *pf)
{
	int vector = pci_irq_vector(pf->pdev, RVU_PF_INT_VEC_AFPF_MBOX);

	/* Disable AF => PF mailbox IRQ */
	otx2_write64(pf, RVU_PF_INT_ENA_W1C, BIT_ULL(0));
	free_irq(vector, pf);
}

static int otx2_register_mbox_intr(struct otx2_nic *pf)
{
	struct otx2_hw *hw = &pf->hw;
	struct msg_req *req;
	char *irq_name;
	int err;

	/* Register mailbox interrupt handler */
	irq_name = &hw->irq_name[RVU_PF_INT_VEC_AFPF_MBOX * NAME_SIZE];
	snprintf(irq_name, NAME_SIZE, "RVUPFAF Mbox");
	err = request_irq(pci_irq_vector(pf->pdev, RVU_PF_INT_VEC_AFPF_MBOX),
			  otx2_pfaf_mbox_intr_handler, 0, irq_name, pf);
	if (err) {
		dev_err(pf->dev,
			"RVUPF: IRQ registration failed for PFAF mbox irq\n");
		return err;
	}

	/* Enable mailbox interrupt for msgs coming from AF.
	 * First clear to avoid spurious interrupts, if any.
	 */
	otx2_write64(pf, RVU_PF_INT, BIT_ULL(0));
	otx2_write64(pf, RVU_PF_INT_ENA_W1S, BIT_ULL(0));

	/* Check mailbox communication with AF */
	req = otx2_mbox_alloc_msg_ready(&pf->mbox);
	if (!req) {
		otx2_disable_mbox_intr(pf);
		return -ENOMEM;
	}
	err = otx2_sync_mbox_msg(&pf->mbox);
	if (err) {
		dev_warn(pf->dev,
			 "AF not responding to mailbox, deferring probe\n");
		otx2_disable_mbox_intr(pf);
		return -EPROBE_DEFER;
	}

	return 0;
}

static void otx2_pfaf_mbox_destroy(struct otx2_nic *pf)
{
	struct mbox *mbox = &pf->mbox;

	if (pf->mbox_wq) {
		flush_workqueue(pf->mbox_wq);
		destroy_workqueue(pf->mbox_wq);
		pf->mbox_wq = NULL;
	}

	if (mbox->mbox.hwbase)
		iounmap((void __iomem *)mbox->mbox.hwbase);

	otx2_mbox_destroy(&mbox->mbox);
	otx2_mbox_destroy(&mbox->mbox_up);
}

static int otx2_pfaf_mbox_init(struct otx2_nic *pf)
{
	struct mbox *mbox = &pf->mbox;
	void __iomem *hwbase;
	int err;

	mbox->pfvf = pf;
	pf->mbox_wq = alloc_workqueue("otx2_pfaf_mailbox",
				      WQ_UNBOUND | WQ_HIGHPRI |
				      WQ_MEM_RECLAIM, 1);
	if (!pf->mbox_wq)
		return -ENOMEM;

	/* Mailbox is a reserved memory (in RAM) region shared between
	 * admin function (i.e AF) and this PF, shouldn't be mapped as
	 * device memory to allow unaligned accesses.
	 */
	hwbase = ioremap_wc(pci_resource_start(pf->pdev, PCI_MBOX_BAR_NUM),
			    pci_resource_len(pf->pdev, PCI_MBOX_BAR_NUM));
	if (!hwbase) {
		dev_err(pf->dev, "Unable to map PFAF mailbox region\n");
		err = -ENOMEM;
		goto exit;
	}

	err = otx2_mbox_init(&mbox->mbox, hwbase, pf->pdev, pf->reg_base,
			     MBOX_DIR_PFAF, 1);
	if (err)
		goto exit;

	err = otx2_mbox_init(&mbox->mbox_up, hwbase, pf->pdev, pf->reg_base,
			     MBOX_DIR_PFAF_UP, 1);
	if (err)
		goto exit;

	err = otx2_mbox_bbuf_init(mbox, pf->pdev);
	if (err)
		goto exit;

	INIT_WORK(&mbox->mbox_wrk, otx2_pfaf_mbox_handler);
	INIT_WORK(&mbox->mbox_up_wrk, otx2_pfaf_mbox_up_handler);
	otx2_mbox_lock_init(&pf->mbox);

	return 0;
exit:
	destroy_workqueue(pf->mbox_wq);
	return err;
}

static int otx2_cgx_config_linkevents(struct otx2_nic *pf, bool enable)
{
	struct msg_req *msg;
	int err;

	otx2_mbox_lock(&pf->mbox);
	if (enable)
		msg = otx2_mbox_alloc_msg_cgx_start_linkevents(&pf->mbox);
	else
		msg = otx2_mbox_alloc_msg_cgx_stop_linkevents(&pf->mbox);

	if (!msg) {
		otx2_mbox_unlock(&pf->mbox);
		return -ENOMEM;
	}

	err = otx2_sync_mbox_msg(&pf->mbox);
	otx2_mbox_unlock(&pf->mbox);
	return err;
}

static int otx2_cgx_config_loopback(struct otx2_nic *pf, bool enable)
{
	struct msg_req *msg;
	int err;

	otx2_mbox_lock(&pf->mbox);
	if (enable)
		msg = otx2_mbox_alloc_msg_cgx_intlbk_enable(&pf->mbox);
	else
		msg = otx2_mbox_alloc_msg_cgx_intlbk_disable(&pf->mbox);

	if (!msg) {
		otx2_mbox_unlock(&pf->mbox);
		return -ENOMEM;
	}

	err = otx2_sync_mbox_msg(&pf->mbox);
	otx2_mbox_unlock(&pf->mbox);
	return err;
}

static int otx2_enable_rxvlan(struct otx2_nic *pf, bool enable)
{
	struct nix_vtag_config *req;
	struct mbox_msghdr *rsp_hdr;
	int err;

	if (enable) {
		err = otx2_install_rxvlan_offload_flow(pf);
		if (err)
			return err;
	} else {
		err = otx2_delete_rxvlan_offload_flow(pf);
		if (err)
			return err;
	}

	otx2_mbox_lock(&pf->mbox);
	req = otx2_mbox_alloc_msg_nix_vtag_cfg(&pf->mbox);
	if (!req) {
		otx2_mbox_unlock(&pf->mbox);
		return -ENOMEM;
	}

	req->vtag_size = 0;
	req->cfg_type = 1;
	/* must be set to zero */
	req->rx.vtag_type = 0;
	req->rx.strip_vtag = enable;
	req->rx.capture_vtag = enable;

	err = otx2_sync_mbox_msg(&pf->mbox);
	if (err) {
		otx2_mbox_unlock(&pf->mbox);
		return err;
	}

	rsp_hdr = otx2_mbox_get_rsp(&pf->mbox.mbox, 0, &req->hdr);
	if (IS_ERR(rsp_hdr)) {
		otx2_mbox_unlock(&pf->mbox);
		return PTR_ERR(rsp_hdr);
	}

	otx2_mbox_unlock(&pf->mbox);
	return rsp_hdr->rc;
}

int otx2_set_real_num_queues(struct net_device *netdev,
			     int tx_queues, int rx_queues)
{
	int err;

	err = netif_set_real_num_tx_queues(netdev, tx_queues);
	if (err) {
		netdev_err(netdev,
			   "Failed to set no of Tx queues: %d\n", tx_queues);
		return err;
	}

	err = netif_set_real_num_rx_queues(netdev, rx_queues);
	if (err)
		netdev_err(netdev,
			   "Failed to set no of Rx queues: %d\n", rx_queues);
	return err;
}
EXPORT_SYMBOL(otx2_set_real_num_queues);

static void otx2_alloc_rxvlan(struct otx2_nic *pf)
{
	netdev_features_t old, wanted = NETIF_F_HW_VLAN_STAG_RX |
					NETIF_F_HW_VLAN_CTAG_RX;
	struct npc_mcam_alloc_entry_req *req;
	struct npc_mcam_alloc_entry_rsp *rsp;
	int err;

	otx2_mbox_lock(&pf->mbox);
	req = otx2_mbox_alloc_msg_npc_mcam_alloc_entry(&pf->mbox);
	if (!req) {
		otx2_mbox_unlock(&pf->mbox);
		return;
	}

	req->contig = false;
	req->count = 1;
	/* Send message to AF */
	err = otx2_sync_mbox_msg(&pf->mbox);
	if (err) {
		otx2_mbox_unlock(&pf->mbox);
		return;
	}
	rsp = (struct npc_mcam_alloc_entry_rsp *)otx2_mbox_get_rsp
						 (&pf->mbox.mbox, 0, &req->hdr);
	if (IS_ERR(rsp)) {
		otx2_mbox_unlock(&pf->mbox);
		return;
	}

	otx2_mbox_unlock(&pf->mbox);
	pf->rxvlan_entry = rsp->entry_list[0];
	pf->rxvlan_alloc = true;

	old = pf->netdev->hw_features;
	if (rsp->hdr.rc) {
		/* in case of failure during rxvlan allocation
		 * features must be updated accordingly
		 */
		dev_info(pf->dev,
			 "Disabling RX VLAN offload due to non-availability of MCAM space\n");
		pf->netdev->hw_features &= ~wanted;
		pf->netdev->features &= ~wanted;
	} else if (!(pf->netdev->hw_features & wanted)) {
		/* we are recovering from the previous failure */
		pf->netdev->hw_features |= wanted;
		err = otx2_enable_rxvlan(pf, true);
		if (!err)
			pf->netdev->features |= wanted;
	} else if (pf->netdev->features & wanted) {
		/* interface is going up */
		err = otx2_enable_rxvlan(pf, true);
		if (err) {
			pf->netdev->features &= ~wanted;
			netdev_features_change(pf->netdev);
		}
	}

	if (old != pf->netdev->hw_features)
		netdev_features_change(pf->netdev);
}

static irqreturn_t otx2_q_intr_handler(int irq, void *data)
{
	struct otx2_nic *pf = data;
	atomic64_t *ptr;
	u64 qidx = 0;
	u64 val;

	/* CQ */
	for (qidx = 0; qidx < pf->qset.cq_cnt; qidx++) {
		ptr = pf->reg_base + NIX_LF_CQ_OP_INT;
		val = atomic64_fetch_add_relaxed((qidx << 44) |
						 NIX_CQERRINT_BITS, ptr);

		if (!(val & (NIX_CQERRINT_BITS | BIT_ULL(42))))
			continue;

		if (val & BIT_ULL(42)) {
			dev_err(pf->dev, "CQ%lld: error reading NIX_LF_CQ_OP_INT\n",
				qidx);
		} else {
			if (val & BIT_ULL(NIX_CQERRINT_DOOR_ERR))
				dev_err(pf->dev, "CQ%lld: Doorbell error",
					qidx);
			if (val & BIT_ULL(NIX_CQERRINT_WR_FULL))
				dev_err(pf->dev, "CQ%lld: Write full. A CQE to be added has been dropped because the CQ is full",
					qidx);
			if (val & BIT_ULL(NIX_CQERRINT_CQE_FAULT))
				dev_err(pf->dev, "CQ%lld: Memory fault on CQE write to LLC/DRAM",
					qidx);
		}

		schedule_work(&pf->reset_task);
	}

	/* SQ */
	for (qidx = 0; qidx < pf->hw.tx_queues; qidx++) {
		ptr = pf->reg_base + NIX_LF_SQ_OP_INT;
		val = atomic64_fetch_add_relaxed((qidx << 44) | NIX_SQINT_BITS,
						 ptr);
		if (!(val & (NIX_SQINT_BITS | BIT_ULL(42))))
			continue;

		if (val & BIT_ULL(42)) {
			dev_err(pf->dev, "SQ%lld: error reading NIX_LF_SQ_OP_INT\n",
				qidx);
		} else {
			if (val & BIT_ULL(NIX_SQINT_LMT_ERR))
				dev_err(pf->dev, "SQ%lld: LMT store error",
					qidx);
			if (val & BIT_ULL(NIX_SQINT_MNQ_ERR))
				dev_err(pf->dev, "SQ%lld: Meta-descriptor enqueue error",
					qidx);
			if (val & BIT_ULL(NIX_SQINT_SEND_ERR))
				dev_err(pf->dev, "SQ%lld: Send error", qidx);
			if (val & BIT_ULL(NIX_SQINT_SQB_ALLOC_FAIL))
				dev_err(pf->dev, "SQ%lld: SQB allocation failed",
					qidx);
		}

		schedule_work(&pf->reset_task);
	}

	return IRQ_HANDLED;
}

static irqreturn_t otx2_cq_intr_handler(int irq, void *cq_irq)
{
	struct otx2_cq_poll *cq_poll = (struct otx2_cq_poll *)cq_irq;
	struct otx2_nic *pf = (struct otx2_nic *)cq_poll->dev;
	int qidx = cq_poll->cint_idx;

	/* Disable interrupts.
	 *
	 * Completion interrupts behave in a level-triggered interrupt
	 * fashion, and hence have to be cleared only after it is serviced.
	 */
	otx2_write64(pf, NIX_LF_CINTX_ENA_W1C(qidx), BIT_ULL(0));

	/* Schedule NAPI */
	napi_schedule_irqoff(&cq_poll->napi);

	return IRQ_HANDLED;
}

static void otx2_disable_napi(struct otx2_nic *pf)
{
	struct otx2_qset *qset = &pf->qset;
	struct otx2_cq_poll *cq_poll;
	int qidx;

	for (qidx = 0; qidx < pf->hw.cint_cnt; qidx++) {
		cq_poll = &qset->napi[qidx];
		napi_disable(&cq_poll->napi);
		netif_napi_del(&cq_poll->napi);
	}
}

static int otx2_init_hw_resources(struct otx2_nic *pf)
{
	struct otx2_hw *hw = &pf->hw;
	int err = 0, lvl;

	/* Set required NPA LF's pool counts
	 * Auras and Pools are used in a 1:1 mapping,
	 * so, aura count = pool count.
	 */
	hw->rqpool_cnt = hw->rx_queues;
	hw->sqpool_cnt = hw->tx_queues;
	hw->pool_cnt = hw->rqpool_cnt + hw->sqpool_cnt;

	otx2_mbox_lock(&pf->mbox);
	/* NPA init */
	err = otx2_config_npa(pf);
	if (err)
		goto exit;

	/* NIX init */
	err = otx2_config_nix(pf);
	if (err)
		goto exit;

	/* Enable backpressure */
	otx2_nix_config_bp(pf, true);

	/* Init Auras and pools used by NIX RQ, for free buffer ptrs */
	err = otx2_rq_aura_pool_init(pf);
	if (err)
		goto exit;

	/* Init Auras and pools used by NIX SQ, for queueing SQEs */
	err = otx2_sq_aura_pool_init(pf);
	if (err)
		goto exit;

	err = otx2_txsch_alloc(pf);
	if (err)
		goto exit;

	err = otx2_config_nix_queues(pf);
	if (err)
		goto exit;

	for (lvl = 0; lvl < NIX_TXSCH_LVL_CNT; lvl++) {
		err = otx2_txschq_config(pf, lvl);
		if (err)
			goto exit;
	}
exit:
	otx2_mbox_unlock(&pf->mbox);
	return err;
}

static void otx2_free_hw_resources(struct otx2_nic *pf)
{
	struct otx2_qset *qset = &pf->qset;
	struct mbox *mbox = &pf->mbox;
	struct otx2_snd_queue *sq;
	struct otx2_cq_queue *cq;
	int err, qidx, cqe_count;
	struct msg_req *req;

	/* Stop transmission */
	err = otx2_txschq_stop(pf);
	if (err)
		dev_err(pf->dev, "RVUPF: Failed to stop/free TX schedulers\n");

	otx2_mbox_lock(mbox);
	/* Disable backpressure */
	if (!(pf->pcifunc & RVU_PFVF_FUNC_MASK))
		otx2_nix_config_bp(pf, false);
	otx2_mbox_unlock(mbox);

	/* Disable SQs */
	otx2_ctx_disable(mbox, NIX_AQ_CTYPE_SQ, false);
	for (qidx = 0; qidx < pf->hw.tx_queues; qidx++) {
		sq = &qset->sq[qidx];
		qmem_free(pf->dev, sq->sqe);
		if (!pf->hw.hw_tso)
			qmem_free(pf->dev, sq->tso_hdrs);
		kfree(sq->sg);
		qmem_free(pf->dev, sq->timestamps);
	}

	/* Free SQB pointers */
	otx2_free_aura_ptr(pf, AURA_NIX_SQ);

	/* Disable RQs */
	otx2_ctx_disable(mbox, NIX_AQ_CTYPE_RQ, false);

	/*Dequeue all CQEs */
	for (qidx = 0; qidx < qset->cq_cnt; qidx++) {
		cq = &qset->cq[qidx];
		cqe_count = otx2_read64(pf, NIX_LF_CINTX_CNT(cq->cint_idx));
		cqe_count &= 0xFFFFFFFF;
		if (cqe_count)
			otx2_napi_handler(cq, pf, cqe_count);
	}

	/* Free RQ buffer pointers*/
	otx2_free_aura_ptr(pf, AURA_NIX_RQ);

	/* Disable CQs*/
	otx2_ctx_disable(mbox, NIX_AQ_CTYPE_CQ, false);
	for (qidx = 0; qidx < qset->cq_cnt; qidx++) {
		cq = &qset->cq[qidx];
		qmem_free(pf->dev, cq->cqe);
	}

	otx2_mbox_lock(mbox);
	/* Reset NIX LF */
	req = otx2_mbox_alloc_msg_nix_lf_free(mbox);
	if (req)
		WARN_ON(otx2_sync_mbox_msg(mbox));
	otx2_mbox_unlock(mbox);

	/* Disable NPA Pool and Aura hw context */
	otx2_ctx_disable(mbox, NPA_AQ_CTYPE_POOL, true);
	otx2_ctx_disable(mbox, NPA_AQ_CTYPE_AURA, true);
	otx2_aura_pool_free(pf);

	otx2_mbox_lock(mbox);
	/* Reset NPA LF */
	req = otx2_mbox_alloc_msg_npa_lf_free(mbox);
	if (req)
		WARN_ON(otx2_sync_mbox_msg(mbox));
	otx2_mbox_unlock(mbox);
}

static netdev_tx_t otx2_xmit(struct sk_buff *skb, struct net_device *netdev)

{
	struct otx2_nic *pf = netdev_priv(netdev);
	struct otx2_snd_queue *sq;
	int qidx = skb_get_queue_mapping(skb);
	struct netdev_queue *txq = netdev_get_tx_queue(netdev, qidx);

	/* Check for minimum packet length */
	if (skb->len <= ETH_HLEN) {
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	sq = &pf->qset.sq[qidx];

	if (!netif_tx_queue_stopped(txq) &&
	    !otx2_sq_append_skb(netdev, sq, skb, qidx)) {
		netif_tx_stop_queue(txq);

		/* Barrier, for stop_queue to be visible on other cpus */
		smp_mb();
		if ((sq->num_sqbs - *sq->aura_fc_addr) > 1)
			netif_tx_start_queue(txq);
		else
			netdev_warn(netdev,
				    "%s: No free SQE/SQB, stopping SQ%d\n",
				     netdev->name, qidx);

		return NETDEV_TX_BUSY;
	}

	return NETDEV_TX_OK;
}

int otx2_open(struct net_device *netdev)
{
	struct otx2_nic *pf = netdev_priv(netdev);
	struct otx2_cq_poll *cq_poll = NULL;
	struct otx2_qset *qset = &pf->qset;
	int err = 0, qidx, vec;
	char *irq_name;

	netif_carrier_off(netdev);

	pf->qset.cq_cnt = pf->hw.rx_queues + pf->hw.tx_queues;
	/* RQ and SQs are mapped to different CQs,
	 * so find out max CQ IRQs (i.e CINTs) needed.
	 */
	pf->hw.cint_cnt = max(pf->hw.rx_queues, pf->hw.tx_queues);
	qset->napi = kcalloc(pf->hw.cint_cnt, sizeof(*cq_poll), GFP_KERNEL);
	if (!qset->napi)
		return -ENOMEM;

	/* CQ size of RQ */
	qset->rqe_cnt = qset->rqe_cnt ? qset->rqe_cnt : Q_COUNT(Q_SIZE_1K);
	/* CQ size of SQ */
	qset->sqe_cnt = qset->sqe_cnt ? qset->sqe_cnt : Q_COUNT(Q_SIZE_4K);

	err = -ENOMEM;
	qset->cq = kcalloc(pf->qset.cq_cnt,
			   sizeof(struct otx2_cq_queue), GFP_KERNEL);
	if (!qset->cq)
		goto err_free_mem;

	qset->sq = kcalloc(pf->hw.tx_queues,
			   sizeof(struct otx2_snd_queue), GFP_KERNEL);
	if (!qset->sq)
		goto err_free_mem;

	qset->rq = kcalloc(pf->hw.rx_queues,
			   sizeof(struct otx2_rcv_queue), GFP_KERNEL);
	if (!qset->rq)
		goto err_free_mem;

	err = otx2_init_hw_resources(pf);
	if (err)
		goto err_free_mem;

	/* Register NAPI handler */
	for (qidx = 0; qidx < pf->hw.cint_cnt; qidx++) {
		cq_poll = &qset->napi[qidx];
		cq_poll->cint_idx = qidx;
		/* RQ0 & SQ0 are mapped to CINT0 and so on..
		 * 'cq_ids[0]' points to RQ's CQ and
		 * 'cq_ids[1]' points to SQ's CQ and
		 */
		cq_poll->cq_ids[0] =
			(qidx <  pf->hw.rx_queues) ? qidx : CINT_INVALID_CQ;
		cq_poll->cq_ids[1] = (qidx < pf->hw.tx_queues) ?
				      qidx + pf->hw.rx_queues : CINT_INVALID_CQ;
		cq_poll->dev = (void *)pf;
		netif_napi_add(netdev, &cq_poll->napi,
			       otx2_poll, NAPI_POLL_WEIGHT);
		napi_enable(&cq_poll->napi);
	}

	/* Set default mac address */
	err = otx2_hw_set_mac_addr(pf, netdev);
	if (err)
		goto err_disable_napi;

	/* Set default MTU in HW */
	err = otx2_hw_set_mtu(pf, netdev->mtu);
	if (err)
		goto err_disable_napi;

	/* Initialize RSS */
	err = otx2_rss_init(pf);
	if (err)
		goto err_disable_napi;

	/* Register Queue IRQ handlers */
	vec = pf->hw.nix_msixoff + NIX_LF_QINT_VEC_START;
	irq_name = &pf->hw.irq_name[vec * NAME_SIZE];

	snprintf(irq_name, NAME_SIZE, "%s-qerr", pf->netdev->name);

	err = request_irq(pci_irq_vector(pf->pdev, vec),
			  otx2_q_intr_handler, 0, irq_name, pf);
	if (err) {
		dev_err(pf->dev,
			"RVUPF%d: IRQ registration failed for QERR\n",
			rvu_get_pf(pf->pcifunc));
		goto err_disable_napi;
	}

	/* Enable QINT IRQ */
	otx2_write64(pf, NIX_LF_QINTX_ENA_W1S(0), BIT_ULL(0));

	/* Register CQ IRQ handlers */
	vec = pf->hw.nix_msixoff + NIX_LF_CINT_VEC_START;
	for (qidx = 0; qidx < pf->hw.cint_cnt; qidx++) {
		irq_name = &pf->hw.irq_name[vec * NAME_SIZE];

		snprintf(irq_name, NAME_SIZE, "%s-rxtx-%d", pf->netdev->name,
			 qidx);

		err = request_irq(pci_irq_vector(pf->pdev, vec),
				  otx2_cq_intr_handler, 0, irq_name,
				  &qset->napi[qidx]);
		if (err) {
			dev_err(pf->dev,
				"RVUPF%d: IRQ registration failed for CQ%d\n",
				rvu_get_pf(pf->pcifunc), qidx);
			goto err_free_cints;
		}
		vec++;

		/* Configure CQE interrupt coalescing parameters */
		otx2_write64(pf, NIX_LF_CINTX_WAIT(qidx),
			     ((u64)pf->cq_time_wait << 48) |
			     pf->cq_ecount_wait);

		/* Enable CQ IRQ */
		otx2_write64(pf, NIX_LF_CINTX_INT(qidx), BIT_ULL(0));
		otx2_write64(pf, NIX_LF_CINTX_ENA_W1S(qidx), BIT_ULL(0));
	}

	otx2_set_cints_affinity(pf);

	err = otx2_rxtx_enable(pf, true);
	if (err)
		goto err_free_cints;

	pf->intf_down = false;

	/* we have already received link status notification */
	if (pf->linfo.link_up)
		otx2_handle_link_event(pf);

	/* Alloc rxvlan entry in MCAM for PFs only */
	if (!(pf->pcifunc & RVU_PFVF_FUNC_MASK))
		otx2_alloc_rxvlan(pf);

	/* When reinitializing enable time stamping if it is enabled before */
	if (pf->hw_tx_tstamp) {
		pf->hw_tx_tstamp = 0;
		otx2_config_hw_tx_tstamp(pf, true);
	}
	if (pf->hw_rx_tstamp) {
		pf->hw_rx_tstamp = 0;
		otx2_config_hw_rx_tstamp(pf, true);
	}

	return 0;

err_free_cints:
	otx2_free_cints(pf, qidx);
	vec = pci_irq_vector(pf->pdev,
			     pf->hw.nix_msixoff + NIX_LF_QINT_VEC_START);
	otx2_write64(pf, NIX_LF_QINTX_ENA_W1C(0), BIT_ULL(0));
	synchronize_irq(vec);
	free_irq(vec, pf);
err_disable_napi:
	otx2_disable_napi(pf);
	otx2_free_hw_resources(pf);
err_free_mem:
	kfree(qset->sq);
	kfree(qset->cq);
	kfree(qset->napi);
	return err;
}
EXPORT_SYMBOL(otx2_open);

int otx2_stop(struct net_device *netdev)
{
	struct otx2_nic *pf = netdev_priv(netdev);
	struct otx2_cq_poll *cq_poll = NULL;
	struct otx2_qset *qset = &pf->qset;
	int qidx, vec;

	/* First stop packet Rx/Tx */
	otx2_rxtx_enable(pf, false);

	otx2_destroy_ethtool_flows(pf);

	pf->intf_down = true;
	/* 'intf_down' may be checked on any cpu */
	smp_wmb();

	netif_carrier_off(netdev);
	netif_tx_stop_all_queues(netdev);

	/* Cleanup Queue IRQ */
	vec = pci_irq_vector(pf->pdev,
			     pf->hw.nix_msixoff + NIX_LF_QINT_VEC_START);
	otx2_write64(pf, NIX_LF_QINTX_ENA_W1C(0), BIT_ULL(0));
	synchronize_irq(vec);
	free_irq(vec, pf);

	/* Cleanup CQ NAPI and IRQ */
	vec = pf->hw.nix_msixoff + NIX_LF_CINT_VEC_START;
	for (qidx = 0; qidx < pf->hw.cint_cnt; qidx++) {
		/* Disable interrupt */
		otx2_write64(pf, NIX_LF_CINTX_ENA_W1C(qidx), BIT_ULL(0));

		synchronize_irq(pci_irq_vector(pf->pdev, vec));

		cq_poll = &qset->napi[qidx];
		napi_synchronize(&cq_poll->napi);
		vec++;
	}

	netif_tx_disable(netdev);

	otx2_free_hw_resources(pf);
	otx2_free_cints(pf, pf->hw.cint_cnt);
	otx2_disable_napi(pf);

	for (qidx = 0; qidx < netdev->num_tx_queues; qidx++)
		netdev_tx_reset_queue(netdev_get_tx_queue(netdev, qidx));

	kfree(qset->sq);
	kfree(qset->cq);
	kfree(qset->napi);
	memset(qset, 0, sizeof(*qset));
	return 0;
}
EXPORT_SYMBOL(otx2_stop);

static netdev_features_t otx2_fix_features(struct net_device *dev,
					   netdev_features_t features)
{
	if (features & NETIF_F_HW_VLAN_CTAG_RX)
		features |= NETIF_F_HW_VLAN_STAG_RX;
	else
		features &= ~NETIF_F_HW_VLAN_STAG_RX;

	return features;
}

static void otx2_set_rx_mode(struct net_device *netdev)
{
	struct otx2_nic *pf = netdev_priv(netdev);
	struct nix_rx_mode *req;

	if (!(netdev->flags & IFF_UP))
		return;

	otx2_mbox_lock(&pf->mbox);
	req = otx2_mbox_alloc_msg_nix_set_rx_mode(&pf->mbox);
	if (!req) {
		otx2_mbox_unlock(&pf->mbox);
		return;
	}

	req->mode = NIX_RX_MODE_UCAST;

	/* We don't support MAC address filtering yet */
	if (netdev->flags & IFF_PROMISC)
		req->mode |= NIX_RX_MODE_PROMISC;
	else if (netdev->flags & (IFF_ALLMULTI | IFF_MULTICAST))
		req->mode |= NIX_RX_MODE_ALLMULTI;

	otx2_sync_mbox_msg_busy_poll(&pf->mbox);
	otx2_mbox_unlock(&pf->mbox);
}

static void otx2_reset_task(struct work_struct *work)
{
	struct otx2_nic *pf = container_of(work, struct otx2_nic, reset_task);

	if (!netif_running(pf->netdev))
		return;

	otx2_stop(pf->netdev);
	pf->reset_count++;
	otx2_open(pf->netdev);
	netif_trans_update(pf->netdev);
}

static int otx2_set_features(struct net_device *netdev,
			     netdev_features_t features)
{
	struct otx2_nic *pf = netdev_priv(netdev);
	netdev_features_t changed = features ^ netdev->features;
	bool ntuple = !!(features & NETIF_F_NTUPLE);

	if ((changed & NETIF_F_LOOPBACK) && netif_running(netdev))
		return otx2_cgx_config_loopback(pf,
						features & NETIF_F_LOOPBACK);

	if ((changed & NETIF_F_HW_VLAN_CTAG_RX) && netif_running(netdev))
		return otx2_enable_rxvlan(pf,
					  features & NETIF_F_HW_VLAN_CTAG_RX);

	if ((changed & NETIF_F_NTUPLE) && !ntuple)
		otx2_destroy_ethtool_flows(pf);

	return 0;
}

static int otx2_config_hw_rx_tstamp(struct otx2_nic *pfvf, bool enable)
{
	struct msg_req *req;
	int err;

	if (!!pfvf->hw_rx_tstamp == enable)
		return 0;

	otx2_mbox_lock(&pfvf->mbox);
	if (enable)
		req = otx2_mbox_alloc_msg_cgx_ptp_rx_enable(&pfvf->mbox);
	else
		req = otx2_mbox_alloc_msg_cgx_ptp_rx_disable(&pfvf->mbox);
	if (!req) {
		otx2_mbox_unlock(&pfvf->mbox);
		return -ENOMEM;
	}

	err = otx2_sync_mbox_msg(&pfvf->mbox);
	if (err) {
		otx2_mbox_unlock(&pfvf->mbox);
		return err;
	}

	otx2_mbox_unlock(&pfvf->mbox);
	pfvf->hw_rx_tstamp = enable;
	return 0;
}

static int otx2_config_hw_tx_tstamp(struct otx2_nic *pfvf, bool enable)
{
	struct msg_req *req;
	int err;

	if (!!pfvf->hw_tx_tstamp == enable)
		return 0;

	otx2_mbox_lock(&pfvf->mbox);
	if (enable)
		req = otx2_mbox_alloc_msg_nix_lf_ptp_tx_enable(&pfvf->mbox);
	else
		req = otx2_mbox_alloc_msg_nix_lf_ptp_tx_disable(&pfvf->mbox);
	if (!req) {
		otx2_mbox_unlock(&pfvf->mbox);
		return -ENOMEM;
	}

	err = otx2_sync_mbox_msg(&pfvf->mbox);
	if (err) {
		otx2_mbox_unlock(&pfvf->mbox);
		return err;
	}

	otx2_mbox_unlock(&pfvf->mbox);
	pfvf->hw_tx_tstamp = enable;
	return 0;
}

static int otx2_config_hwtstamp(struct net_device *netdev, struct ifreq *ifr)
{
	struct otx2_nic *pfvf = netdev_priv(netdev);
	struct hwtstamp_config config;

	if (!pfvf->ptp)
		return -ENODEV;

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	/* reserved for future extensions */
	if (config.flags)
		return -EINVAL;

	switch (config.tx_type) {
	case HWTSTAMP_TX_OFF:
		otx2_config_hw_tx_tstamp(pfvf, false);
		break;
	case HWTSTAMP_TX_ON:
		otx2_config_hw_tx_tstamp(pfvf, true);
		break;
	default:
		return -ERANGE;
	}

	switch (config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		otx2_config_hw_rx_tstamp(pfvf, false);
		break;
	case HWTSTAMP_FILTER_ALL:
	case HWTSTAMP_FILTER_SOME:
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		otx2_config_hw_rx_tstamp(pfvf, true);
		config.rx_filter = HWTSTAMP_FILTER_ALL;
		break;
	default:
		return -ERANGE;
	}

	if (copy_to_user(ifr->ifr_data, &config, sizeof(config)))
		return -EFAULT;

	return 0;
}

static int otx2_ioctl(struct net_device *netdev, struct ifreq *req, int cmd)
{
	switch (cmd) {
	case SIOCSHWTSTAMP:
		return otx2_config_hwtstamp(netdev, req);
	default:
		return -EOPNOTSUPP;
	}
}

static int otx2_do_set_vf_mac(struct otx2_nic *pf, int vf, const u8 *mac)
{
	struct npc_install_flow_req *req;
	int err;

	otx2_mbox_lock(&pf->mbox);
	req = otx2_mbox_alloc_msg_npc_install_flow(&pf->mbox);
	if (!req) {
		otx2_mbox_unlock(&pf->mbox);
		return -ENOMEM;
	}

	ether_addr_copy(req->packet.dmac, mac);
	u64_to_ether_addr(0xffffffffffffull, req->mask.dmac);
	req->features = BIT_ULL(NPC_DMAC);
	req->channel = pf->rx_chan_base;
	req->intf = NIX_INTF_RX;
	req->default_rule = 1;
	req->append = 1;
	req->vf = vf + 1;
	req->op = NIX_RX_ACTION_DEFAULT;

	err = otx2_sync_mbox_msg(&pf->mbox);
	otx2_mbox_unlock(&pf->mbox);

	return err;
}

static int otx2_set_vf_mac(struct net_device *netdev, int vf, u8 *mac)
{
	struct otx2_nic *pf = netdev_priv(netdev);
	struct pci_dev *pdev = pf->pdev;
	struct otx2_vf_config *config;

	if (vf >= pci_num_vf(pdev))
		return -EINVAL;

	if (!is_valid_ether_addr(mac))
		return -EINVAL;

	config = &pf->vf_configs[vf];
	ether_addr_copy(config->mac, mac);
	if (config->intf_down)
		return 0;

	return otx2_do_set_vf_mac(pf, vf, mac);
}

static int otx2_do_set_vf_vlan(struct otx2_nic *pf, int vf, u16 vlan, u8 qos)
{
	struct npc_install_flow_req *req;
	int err;

	otx2_mbox_lock(&pf->mbox);
	req = otx2_mbox_alloc_msg_npc_install_flow(&pf->mbox);
	if (!req) {
		otx2_mbox_unlock(&pf->mbox);
		return -ENOMEM;
	}

	req->packet.vlan_tci = htons(vlan);
	req->mask.vlan_tci = htons(VLAN_VID_MASK);
	req->features = BIT_ULL(NPC_OUTER_VID);
	req->channel = pf->rx_chan_base;
	req->intf = NIX_INTF_RX;
	req->default_rule = 1;
	req->append = 1;
	req->vf = vf + 1;
	req->op = NIX_RX_ACTION_DEFAULT;

	err = otx2_sync_mbox_msg(&pf->mbox);
	otx2_mbox_unlock(&pf->mbox);

	return err;
}

static int otx2_set_vf_vlan(struct net_device *netdev, int vf, u16 vlan, u8 qos,
			    __be16 proto)
{
	struct otx2_nic *pf = netdev_priv(netdev);
	struct pci_dev *pdev = pf->pdev;
	struct otx2_vf_config *config;

	if (vf >= pci_num_vf(pdev))
		return -EINVAL;

	/* qos is currently unsupported */
	if (vlan >= VLAN_N_VID || qos)
		return -EINVAL;

	if (proto != htons(ETH_P_8021Q))
		return -EPROTONOSUPPORT;

	config = &pf->vf_configs[vf];
	config->vlan = vlan;
	if (config->intf_down)
		return 0;

	return otx2_do_set_vf_vlan(pf, vf, vlan, qos);
}

static int otx2_get_vf_config(struct net_device *netdev, int vf,
			      struct ifla_vf_info *ivi)
{
	struct otx2_nic *pf = netdev_priv(netdev);
	struct pci_dev *pdev = pf->pdev;
	struct otx2_vf_config *config;

	if (vf >= pci_num_vf(pdev))
		return -EINVAL;

	config = &pf->vf_configs[vf];
	ivi->vf = vf;
	ether_addr_copy(ivi->mac, config->mac);
	ivi->vlan = config->vlan;

	return 0;
}

static const struct net_device_ops otx2_netdev_ops = {
	.ndo_open		= otx2_open,
	.ndo_stop		= otx2_stop,
	.ndo_start_xmit		= otx2_xmit,
	.ndo_fix_features	= otx2_fix_features,
	.ndo_set_mac_address    = otx2_set_mac_address,
	.ndo_change_mtu		= otx2_change_mtu,
	.ndo_set_rx_mode	= otx2_set_rx_mode,
	.ndo_set_features	= otx2_set_features,
	.ndo_tx_timeout		= otx2_tx_timeout,
	.ndo_get_stats64	= otx2_get_stats64,
	.ndo_do_ioctl		= otx2_ioctl,
	.ndo_set_vf_mac		= otx2_set_vf_mac,
	.ndo_set_vf_vlan	= otx2_set_vf_vlan,
	.ndo_get_vf_config	= otx2_get_vf_config,
};

static int otx2_check_pf_usable(struct otx2_nic *nic)
{
	u64 rev;

	rev = otx2_read64(nic, RVU_PF_BLOCK_ADDRX_DISC(BLKADDR_RVUM));
	rev = (rev >> 12) & 0xFF;
	/* Check if AF has setup revision for RVUM block,
	 * otherwise this driver probe should be deferred
	 * until AF driver comes up.
	 */
	if (!rev) {
		dev_warn(nic->dev,
			 "AF is not initialized, deferring probe\n");
		return -EPROBE_DEFER;
	}
	return 0;
}

static int otx2_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct net_device *netdev;
	struct otx2_nic *pf;
	struct otx2_hw *hw;
	int err, qcount;
	int num_vec;

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(dev, "Failed to enable PCI device\n");
		return err;
	}

	err = pci_request_regions(pdev, DRV_NAME);
	if (err) {
		dev_err(dev, "PCI request regions failed 0x%x\n", err);
		return err;
	}

	err = pci_set_dma_mask(pdev, DMA_BIT_MASK(48));
	if (err) {
		dev_err(dev, "Unable to set DMA mask\n");
		goto err_release_regions;
	}

	err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(48));
	if (err) {
		dev_err(dev, "Unable to set consistent DMA mask\n");
		goto err_release_regions;
	}

	pci_set_master(pdev);

	/* Set number of queues */
	qcount = min_t(int, num_online_cpus(), OTX2_MAX_CQ_CNT);

	netdev = alloc_etherdev_mqs(sizeof(*pf), qcount, qcount);
	if (!netdev) {
		err = -ENOMEM;
		goto err_release_regions;
	}

	pci_set_drvdata(pdev, netdev);
	SET_NETDEV_DEV(netdev, &pdev->dev);
	pf = netdev_priv(netdev);
	pf->netdev = netdev;
	pf->pdev = pdev;
	pf->dev = dev;
	pf->total_vfs = pci_sriov_get_totalvfs(pdev);

	hw = &pf->hw;
	hw->pdev = pdev;
	hw->rx_queues = qcount;
	hw->tx_queues = qcount;
	hw->max_queues = qcount;

	num_vec = pci_msix_vec_count(pdev);
	hw->irq_name = devm_kmalloc_array(&hw->pdev->dev, num_vec, NAME_SIZE,
					  GFP_KERNEL);
	if (!hw->irq_name)
		goto err_free_netdev;

	hw->affinity_mask = devm_kcalloc(&hw->pdev->dev, num_vec,
					 sizeof(cpumask_var_t), GFP_KERNEL);
	if (!hw->affinity_mask)
		goto err_free_netdev;


	/* Map CSRs */
	pf->reg_base = pcim_iomap(pdev, PCI_CFG_REG_BAR_NUM, 0);
	if (!pf->reg_base) {
		dev_err(dev, "Unable to map physical function CSRs, aborting\n");
		err = -ENOMEM;
		goto err_free_netdev;
	}

	err = otx2_check_pf_usable(pf);
	if (err)
		goto err_free_netdev;

	err = pci_alloc_irq_vectors(hw->pdev, num_vec, num_vec, PCI_IRQ_MSIX);
	if (err < 0)
		goto err_free_netdev;
	/* Init PF <=> AF mailbox stuff */
	err = otx2_pfaf_mbox_init(pf);
	if (err)
		goto err_free_irq_vectors;

	/* Register mailbox interrupt */
	err = otx2_register_mbox_intr(pf);
	if (err)
		goto err_mbox_destroy;

	/* Request AF to attach NPA and NIX LFs to this PF.
	 * NIX and NPA LFs are needed for this PF to function as a NIC.
	 */
	err = otx2_attach_npa_nix(pf);
	if (err)
		goto err_disable_mbox_intr;

	err = otx2_set_real_num_queues(netdev, hw->tx_queues, hw->rx_queues);
	if (err)
		goto err_detach_rsrc;

	if (!is_9xxx_pass1_silicon(pdev))
		hw->hw_tso = true;

	pf->cq_time_wait = CQ_TIMER_THRESH_DEFAULT;
	pf->cq_ecount_wait = CQ_CQE_THRESH_DEFAULT;

	/* Don't check for error.  Proceed without ptp */
	otx2_ptp_init(pf);

	/* Assign default mac address */
	otx2_get_mac_from_af(netdev);

	/* NPA's pool is a stack to which SW frees buffer pointers via Aura.
	 * HW allocates buffer pointer from stack and uses it for DMA'ing
	 * ingress packet. In some scenarios HW can free back allocated buffer
	 * pointers to pool. This makes it impossible for SW to maintain a
	 * parallel list where physical addresses of buffer pointers (IOVAs)
	 * given to HW can be saved for later reference.
	 *
	 * So the only way to convert Rx packet's buffer address is to use
	 * IOMMU's iova_to_phys() handler which translates the address by
	 * walking through the translation tables.
	 *
	 * So check if device is binded to IOMMU, otherwise translation is
	 * not needed.
	 */
	pf->iommu_domain = iommu_get_domain_for_dev(dev);

	netdev->hw_features = (NETIF_F_RXCSUM | NETIF_F_IP_CSUM |
			       NETIF_F_IPV6_CSUM | NETIF_F_RXHASH |
			       NETIF_F_SG | NETIF_F_TSO | NETIF_F_TSO6);
	netdev->features |= netdev->hw_features;
	netdev->hw_features |= NETIF_F_LOOPBACK | NETIF_F_NTUPLE |
			       NETIF_F_HW_VLAN_STAG_RX |
			       NETIF_F_HW_VLAN_CTAG_RX;

	netdev->gso_max_segs = OTX2_MAX_GSO_SEGS;
	netdev->watchdog_timeo = OTX2_TX_TIMEOUT;

	netdev->netdev_ops = &otx2_netdev_ops;

	/* MTU range: 68 - 9190 */
	netdev->min_mtu = OTX2_MIN_MTU;
	netdev->max_mtu = OTX2_MAX_MTU;

	INIT_WORK(&pf->reset_task, otx2_reset_task);

	err = register_netdev(netdev);
	if (err) {
		dev_err(dev, "Failed to register netdevice\n");
		goto err_ptp_destroy;
	}

	INIT_LIST_HEAD(&pf->flows);
	pf->max_flows = MAX_ETHTOOL_FLOWS;
	otx2_set_ethtool_ops(netdev);

	/* Enable link notifications */
	otx2_cgx_config_linkevents(pf, true);

	return 0;

err_ptp_destroy:
	otx2_ptp_destroy(pf);
err_detach_rsrc:
	otx2_detach_resources(&pf->mbox);
err_disable_mbox_intr:
	otx2_disable_mbox_intr(pf);
err_mbox_destroy:
	otx2_pfaf_mbox_destroy(pf);
	otx2_pfvf_mbox_destroy(pf);
err_free_irq_vectors:
	pci_free_irq_vectors(hw->pdev);
err_free_netdev:
	pci_set_drvdata(pdev, NULL);
	free_netdev(netdev);
err_release_regions:
	pci_release_regions(pdev);
	return err;
}

static void otx2_vf_link_event_task(struct work_struct *work)
{
	struct otx2_vf_config *config;
	struct cgx_link_info_msg *req;
	struct mbox_msghdr *msghdr;
	struct otx2_nic *pf;
	int vf_idx;

	config = container_of(work, struct otx2_vf_config,
			      link_event_work.work);
	vf_idx = config - config->pf->vf_configs;
	pf = config->pf;

	msghdr = otx2_mbox_alloc_msg_rsp(&pf->mbox_pfvf[0].mbox_up, vf_idx,
					 sizeof(*req), sizeof(struct msg_rsp));
	if (!msghdr) {
		dev_err(pf->dev, "Failed to create VF%d link event\n", vf_idx);
		return;
	}

	req = (struct cgx_link_info_msg *)msghdr;
	req->hdr.id = MBOX_MSG_CGX_LINK_EVENT;
	req->hdr.sig = OTX2_MBOX_REQ_SIG;
	memcpy(&req->link_info, &pf->linfo, sizeof(req->link_info));

	otx2_sync_mbox_up_msg(&pf->mbox_pfvf[0], vf_idx);
}

static void otx2_vf_mac_vlan_task(struct work_struct *work)
{
	struct otx2_vf_config *config;
	struct otx2_nic *pf;
	int vf_idx;

	config = container_of(work, struct otx2_vf_config, mac_vlan_work.work);
	vf_idx = config - config->pf->vf_configs;
	pf = config->pf;

	if (!is_zero_ether_addr(config->mac))
		otx2_do_set_vf_mac(pf, vf_idx, config->mac);

	if (config->vlan)
		otx2_do_set_vf_vlan(pf, vf_idx, config->vlan, 0);
}

static int otx2_sriov_enable(struct pci_dev *pdev, int numvfs)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct otx2_nic *pf = netdev_priv(netdev);
	int ret, i;

	if (numvfs > pf->total_vfs)
		numvfs = pf->total_vfs;

	/* Init PF <=> VF mailbox stuff */
	ret = otx2_pfvf_mbox_init(pf, numvfs);
	if (ret)
		return ret;

	ret = otx2_register_pfvf_mbox_intr(pf);
	if (ret)
		goto free_mbox;

	pf->vf_configs = kcalloc(numvfs, sizeof(struct otx2_vf_config),
				 GFP_KERNEL);
	if (!pf->vf_configs) {
		ret = -ENOMEM;
		goto free_intr;
	}

	for (i = 0; i < numvfs; i++) {
		pf->vf_configs[i].pf = pf;
		pf->vf_configs[i].intf_down = true;
		INIT_DELAYED_WORK(&pf->vf_configs[i].link_event_work,
				  otx2_vf_link_event_task);
		INIT_DELAYED_WORK(&pf->vf_configs[i].mac_vlan_work,
				  otx2_vf_mac_vlan_task);
	}

	ret = otx2_pf_flr_init(pf, numvfs);
	if (ret)
		goto free_configs;

	ret = otx2_register_flr_me_intr(pf);
	if (ret)
		goto free_flr;

	ret = pci_enable_sriov(pdev, numvfs);
	if (ret)
		goto free_flr_intr;

	return numvfs;

free_flr_intr:
	otx2_disable_flr_me_intr(pf);
free_flr:
	otx2_flr_wq_destroy(pf);
free_configs:
	kfree(pf->vf_configs);
free_intr:
	otx2_disable_pfvf_mbox_intr(pf);
free_mbox:
	otx2_pfvf_mbox_destroy(pf);
	return ret;
}

static int otx2_sriov_disable(struct pci_dev *pdev)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct otx2_nic *pf = netdev_priv(netdev);
	int i;

	if (!pci_num_vf(pdev))
		return 0;

	pci_disable_sriov(pdev);

	for (i = 0; i < pci_num_vf(pdev); i++)
		cancel_delayed_work_sync(&pf->vf_configs[i].link_event_work);
	kfree(pf->vf_configs);

	otx2_disable_flr_me_intr(pf);
	otx2_flr_wq_destroy(pf);
	otx2_disable_pfvf_mbox_intr(pf);
	otx2_pfvf_mbox_destroy(pf);
	otx2_delete_vf_ethtool_flows(pf);

	return 0;
}

static int otx2_sriov_configure(struct pci_dev *pdev, int numvfs)
{
	if (numvfs == 0)
		return otx2_sriov_disable(pdev);
	else
		return otx2_sriov_enable(pdev, numvfs);
}

static void otx2_remove(struct pci_dev *pdev)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct otx2_nic *pf;

	if (!netdev)
		return;

	pf = netdev_priv(netdev);

	if (pf->hw_tx_tstamp)
		otx2_config_hw_tx_tstamp(pf, false);
	if (pf->hw_rx_tstamp)
		otx2_config_hw_rx_tstamp(pf, false);

	/* Disable link notifications */
	otx2_cgx_config_linkevents(pf, false);

	unregister_netdev(netdev);
	otx2_ptp_destroy(pf);
	otx2_destroy_ethtool_flows(pf);

	otx2_disable_mbox_intr(pf);

	otx2_detach_resources(&pf->mbox);
	otx2_pfaf_mbox_destroy(pf);
	otx2_sriov_disable(pf->pdev);
	pci_free_irq_vectors(pf->pdev);
	pci_set_drvdata(pdev, NULL);
	free_netdev(netdev);

	pci_release_regions(pdev);
}

static struct pci_driver otx2_pf_driver = {
	.name = DRV_NAME,
	.id_table = otx2_pf_id_table,
	.probe = otx2_probe,
	.remove = otx2_remove,
	.sriov_configure = otx2_sriov_configure
};

static int __init otx2_rvupf_init_module(void)
{
	pr_info("%s: %s\n", DRV_NAME, DRV_STRING);

	return pci_register_driver(&otx2_pf_driver);
}

static void __exit otx2_rvupf_cleanup_module(void)
{
	pci_unregister_driver(&otx2_pf_driver);
}

module_init(otx2_rvupf_init_module);
module_exit(otx2_rvupf_cleanup_module);

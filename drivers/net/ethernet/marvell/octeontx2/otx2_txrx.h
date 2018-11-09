// SPDX-License-Identifier: GPL-2.0
/* Marvell OcteonTx2 RVU Ethernet driver
 *
 * Copyright (C) 2018 Marvell International Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef OTX2_TXRX_H
#define OTX2_TXRX_H

#include <linux/etherdevice.h>
#include <linux/iommu.h>
#include <linux/if_vlan.h>

#define LBK_CHAN_BASE	0x000
#define SDP_CHAN_BASE	0x700
#define CGX_CHAN_BASE	0x800

#define DMA_BUFFER_LEN	1536 /* In multiples of 128bytes */
#define RCV_FRAG_LEN	(SKB_DATA_ALIGN(DMA_BUFFER_LEN + NET_SKB_PAD) + \
			 SKB_DATA_ALIGN(sizeof(struct skb_shared_info)))

#define	OTX2_ETH_HLEN		(VLAN_ETH_HLEN + VLAN_HLEN)
#define OTX2_MIN_MTU		ETH_MIN_MTU
#define OTX2_MAX_MTU		(9212 - OTX2_ETH_HLEN)

#define OTX2_MAX_GSO_SEGS	255
#define OTX2_MAX_FRAGS_IN_SQE	9

#define CQ_CQE_THRESH_DEFAULT	0x0ULL /* IRQ triggered when
					* NIX_LF_CINTX_CNT[QCOUNT]
					* crosses this value
					*/
#define CQ_TIMER_THRESH_DEFAULT	0xAULL /* ~1usec i.e (0xA * 100nsec) */
#define CQ_TIMER_THRESH_MAX     255

struct queue_stats {
	u64	bytes;
	u64	pkts;
};

struct otx2_rcv_queue {
	struct queue_stats	stats;
};

struct sg_list {
	u16	num_segs;
	u64	skb;
	u64	size[OTX2_MAX_FRAGS_IN_SQE];
	u64	dma_addr[OTX2_MAX_FRAGS_IN_SQE];
};

struct otx2_snd_queue {
	u8			aura_id;
	u16			head;
	u16			sqe_size;
	u32			sqe_cnt;
	u16			num_sqbs;
	u8			sqe_per_sqb;
	u64			 io_addr;
	u64			*aura_fc_addr;
	u64			*lmt_addr;
	void			*sqe_base;
	struct qmem		*sqe;
	struct qmem		*tso_hdrs;
	struct sg_list		*sg;
	struct queue_stats	stats;
};

struct otx2_cq_poll {
	void			*dev;
#define CINT_INVALID_CQ		255
#define MAX_CQS_PER_CNT		2 /* RQ + SQ */
	u8			cint_idx;
	u8			cq_ids[MAX_CQS_PER_CNT];
	struct napi_struct	napi;
};

struct otx2_pool {
	struct qmem		*stack;
	struct qmem		*fc_addr;
	u16			rbsize;
	u32			page_offset;
	u16			pageref;
	struct page		*page;
};

struct otx2_cq_queue {
	u8			cq_idx;
	u8			cint_idx; /* CQ interrupt id */
	u16			cqe_size;
	u32			cqe_cnt;
	void			*cqe_base;
	struct qmem		*cqe;
	struct otx2_pool	*rbpool;
};

struct otx2_qset {
#define OTX2_MAX_CQ_CNT		64
	u16			cq_cnt;
	u16			xqe_size;
	u32			rqe_cnt;
	u32			sqe_cnt;
	struct otx2_pool	*pool;
	struct otx2_cq_poll	*napi;
	struct otx2_cq_queue	*cq;
	struct otx2_snd_queue	*sq;
	struct otx2_rcv_queue	*rq;
};

/* Translate IOVA to physical address */
static inline u64 otx2_iova_to_phys(void *iommu_domain, dma_addr_t dma_addr)
{
	/* Translation is installed only when IOMMU is present */
	if (iommu_domain)
		return iommu_iova_to_phys(iommu_domain, dma_addr);
	return dma_addr;
}

int otx2_poll(struct napi_struct *napi, int budget);
bool otx2_sq_append_skb(struct net_device *netdev, struct otx2_snd_queue *sq,
			struct sk_buff *skb, u16 qidx);
#endif /* OTX2_TXRX_H */

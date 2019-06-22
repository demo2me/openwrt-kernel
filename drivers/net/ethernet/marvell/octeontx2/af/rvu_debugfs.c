// SPDX-License-Identifier: GPL-2.0
/* Marvell OcteonTx2 RVU Admin Function driver
 *
 * Copyright (C) 2018 Marvell International Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifdef CONFIG_DEBUG_FS

#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>

#include "rvu_struct.h"
#include "rvu_reg.h"
#include "rvu.h"
#include "cgx.h"
#include "npc.h"

enum {
	CGX_STAT0,
	CGX_STAT1,
	CGX_STAT2,
	CGX_STAT3,
	CGX_STAT4,
	CGX_STAT5,
	CGX_STAT6,
	CGX_STAT7,
	CGX_STAT8,
	CGX_STAT9,
	CGX_STAT10,
	CGX_STAT11,
	CGX_STAT12,
	CGX_STAT13,
	CGX_STAT14,
	CGX_STAT15,
	CGX_STAT16,
	CGX_STAT17,
	CGX_STAT18,
};

static char *cgx_rx_stats_fields[] = {
	[CGX_STAT0]	= "Received packets",
	[CGX_STAT1]	= "Octets of received packets",
	[CGX_STAT2]	= "Received PAUSE packets",
	[CGX_STAT3]	= "Received PAUSE and control packets",
	[CGX_STAT4]	= "Filtered DMAC0 (NIX-bound) packets",
	[CGX_STAT5]	= "Filtered DMAC0 (NIX-bound) octets",
	[CGX_STAT6]	= "Packets dropped due to RX FIFO full",
	[CGX_STAT7]	= "Octets dropped due to RX FIFO full",
	[CGX_STAT8]	= "Error packets",
	[CGX_STAT9]	= "Filtered DMAC1 (NCSI-bound) packets",
	[CGX_STAT10]	= "Filtered DMAC1 (NCSI-bound) octets",
	[CGX_STAT11]	= "NCSI-bound packets dropped",
	[CGX_STAT12]	= "NCSI-bound octets dropped",
};

static char *cgx_tx_stats_fields[] = {
	[CGX_STAT0]	= "Packets dropped due to excessive collisions",
	[CGX_STAT1]	= "Packets dropped due to excessive deferral",
	[CGX_STAT2]	= "Multiple collisions before successful transmission",
	[CGX_STAT3]	= "Single collisions before successful transmission",
	[CGX_STAT4]	= "Total octets sent on the interface",
	[CGX_STAT5]	= "Total frames sent on the interface",
	[CGX_STAT6]	= "Packets sent with an octet count < 64",
	[CGX_STAT7]	= "Packets sent with an octet count == 64",
	[CGX_STAT8]	= "Packets sent with an octet count of 65–127",
	[CGX_STAT9]	= "Packets sent with an octet count of 128-255",
	[CGX_STAT10]	= "Packets sent with an octet count of 256-511",
	[CGX_STAT11]	= "Packets sent with an octet count of 512-1023",
	[CGX_STAT12]	= "Packets sent with an octet count of 1024-1518",
	[CGX_STAT13]	= "Packets sent with an octet count of > 1518",
	[CGX_STAT14]	= "Packets sent to a broadcast DMAC",
	[CGX_STAT15]	= "Packets sent to the multicast DMAC",
	[CGX_STAT16]	= "Transmit underflow and were truncated",
	[CGX_STAT17]	= "Control/PAUSE packets sent",
};

#define NDC_MAX_BANK(rvu, blk_addr) (rvu_read64(rvu, \
		blk_addr, NDC_AF_CONST) & 0xFF)

#define rvu_dbg_NULL NULL
#define rvu_dbg_open_NULL NULL

#define RVU_DEBUG_SEQ_FOPS(name, read_op, write_op)	\
static int rvu_dbg_open_##name(struct inode *inode, struct file *file) \
{ \
	return single_open(file, rvu_dbg_##read_op, inode->i_private); \
} \
static const struct file_operations rvu_dbg_##name##_fops = { \
	.owner		= THIS_MODULE, \
	.open		= rvu_dbg_open_##name, \
	.read		= seq_read, \
	.write		= rvu_dbg_##write_op, \
	.llseek		= seq_lseek, \
	.release	= single_release, \
}

#define RVU_DEBUG_FOPS(name, read_op, write_op) \
static const struct file_operations rvu_dbg_##name##_fops = { \
	.owner = THIS_MODULE, \
	.open = simple_open, \
	.read = rvu_dbg_##read_op, \
	.write = rvu_dbg_##write_op \
}

static void print_nix_qsize(struct seq_file *filp, struct rvu_pfvf *pfvf);

/* Dumps current provisioning status of all RVU block LFs */
static ssize_t rvu_dbg_rsrc_attach_status(struct file *filp,
					  char __user *buffer,
					  size_t count, loff_t *ppos)
{
	int index, off = 0, flag = 0, go_back = 0, off_prev;
	struct rvu *rvu = filp->private_data;
	int lf, pf, vf, pcifunc;
	struct rvu_block block;
	int bytes_not_copied;
	int buf_size = 2048;
	char *buf;

	/* don't allow partial reads */
	if (*ppos != 0)
		return -EINVAL;
	if (count < buf_size)
		return -ENOSPC;

	buf = kzalloc(buf_size, GFP_KERNEL);
	if (!buf)
		return -ENOSPC;
	off +=	scnprintf(&buf[off], buf_size - 1 - off, "\npcifunc\t\t");
	for (index = 0; index < BLK_COUNT; index++)
		if (strlen(rvu->hw->block[index].name))
			off +=	scnprintf(&buf[off], buf_size - 1 - off,
					  "%*s\t", (index - 1) * 2,
					  rvu->hw->block[index].name);
	off += scnprintf(&buf[off], buf_size - 1 - off, "\n");
	for (pf = 0; pf < rvu->hw->total_pfs; pf++) {
		for (vf = 0; vf <= rvu->hw->total_vfs; vf++) {
			pcifunc = pf << 10 | vf;
			if (!pcifunc)
				continue;

			if (vf) {
				go_back = scnprintf(&buf[off],
						    buf_size - 1 - off,
						    "PF%d:VF%d\t\t", pf,
						    vf - 1);
			} else {
				go_back = scnprintf(&buf[off],
						    buf_size - 1 - off,
						    "PF%d\t\t", pf);
			}

			off += go_back;
			for (index = 0; index < BLKTYPE_MAX; index++) {
				block = rvu->hw->block[index];
				if (!strlen(block.name))
					continue;
				off_prev = off;
				for (lf = 0; lf < block.lf.max; lf++) {
					if (block.fn_map[lf] != pcifunc)
						continue;
					flag = 1;
					off += scnprintf(&buf[off], buf_size - 1
							- off, "%3d,", lf);
				}
				if (flag && off_prev != off)
					off--;
				else
					go_back++;
				off += scnprintf(&buf[off], buf_size - 1 - off,
						"\t");
			}
			if (!flag)
				off -= go_back;
			else
				flag = 0;
			off--;
			off +=	scnprintf(&buf[off], buf_size - 1 - off, "\n");
		}
	}

	bytes_not_copied = copy_to_user(buffer, buf, off);
	kfree(buf);

	if (bytes_not_copied)
		return -EFAULT;

	*ppos = off;
	return off;
}

RVU_DEBUG_FOPS(rsrc_status, rsrc_attach_status, NULL);

static bool rvu_dbg_is_valid_lf(struct rvu *rvu, int blktype, int lf,
				u16 *pcifunc)
{
	struct rvu_block *block;
	struct rvu_hwinfo *hw;
	int blkaddr;

	blkaddr = rvu_get_blkaddr(rvu, blktype, 0);
	if (blkaddr < 0) {
		dev_warn(rvu->dev, "Invalid blktype\n");
		return false;
	}

	hw = rvu->hw;
	block = &hw->block[blkaddr];

	if (lf < 0 || lf >= block->lf.max) {
		dev_warn(rvu->dev, "Invalid LF: valid range: 0-%d\n",
			 block->lf.max - 1);
		return false;
	}

	*pcifunc = block->fn_map[lf];
	if (!*pcifunc) {
		dev_warn(rvu->dev,
			 "This LF is not attached to any RVU PFFUNC\n");
		return false;
	}
	return true;
}

static void print_npa_qsize(struct seq_file *m, struct rvu_pfvf *pfvf)
{
	char *buf;

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return;

	if (!pfvf->aura_ctx) {
		seq_puts(m, "Aura context is not initialized\n");
	} else {
		bitmap_print_to_pagebuf(false, buf, pfvf->aura_bmap,
					pfvf->aura_ctx->qsize);
		seq_printf(m, "Aura count : %d\n", pfvf->aura_ctx->qsize);
		seq_printf(m, "Aura context ena/dis bitmap : %s\n", buf);
	}

	if (!pfvf->pool_ctx) {
		seq_puts(m, "Pool context is not initialized\n");
	} else {
		bitmap_print_to_pagebuf(false, buf, pfvf->pool_bmap,
					pfvf->pool_ctx->qsize);
		seq_printf(m, "Pool count : %d\n", pfvf->pool_ctx->qsize);
		seq_printf(m, "Pool context ena/dis bitmap : %s\n", buf);
	}
	kfree(buf);
}

/* The 'qsize' entry dumps current Aura/Pool context Qsize
 * and each context's current enable/disable status in a bitmap.
 */
static int rvu_dbg_qsize_display(struct seq_file *filp, void *unsused,
				 int blktype)
{
	void (*print_qsize)(struct seq_file *filp,
			    struct rvu_pfvf *pfvf) = NULL;
	struct rvu_pfvf *pfvf;
	struct rvu *rvu;
	int qsize_id;
	u16 pcifunc;

	rvu = filp->private;
	switch (blktype) {
	case BLKTYPE_NPA:
		qsize_id = rvu->rvu_dbg.npa_qsize_id;
		print_qsize = print_npa_qsize;
		break;

	case BLKTYPE_NIX:
		qsize_id = rvu->rvu_dbg.nix_qsize_id;
		print_qsize = print_nix_qsize;
		break;

	default:
		return -EINVAL;
	}

	if (!rvu_dbg_is_valid_lf(rvu, blktype, qsize_id, &pcifunc))
		return -EINVAL;

	pfvf = rvu_get_pfvf(rvu, pcifunc);
	print_qsize(filp, pfvf);

	return 0;
}

static ssize_t rvu_dbg_qsize_write(struct file *filp,
				   const char __user *buffer, size_t count,
				   loff_t *ppos, int blktype)
{
	char *blk_string = (blktype == BLKTYPE_NPA) ? "npa" : "nix";
	struct seq_file *seqfile = filp->private_data;
	char *cmd_buf, *cmd_buf_tmp, *subtoken;
	struct rvu *rvu = seqfile->private;
	u16 pcifunc;
	int ret, lf;

	cmd_buf = memdup_user(buffer, count);
	if (IS_ERR(cmd_buf))
		return -ENOMEM;

	cmd_buf[count] = '\0';

	cmd_buf_tmp = strchr(cmd_buf, '\n');
	if (cmd_buf_tmp) {
		*cmd_buf_tmp = '\0';
		count = cmd_buf_tmp - cmd_buf + 1;
	}

	cmd_buf_tmp = cmd_buf;
	subtoken = strsep(&cmd_buf, " ");
	ret = subtoken ? kstrtoint(subtoken, 10, &lf) : -EINVAL;
	if (cmd_buf)
		ret = -EINVAL;

	if (!strncmp(subtoken, "help", 4) || ret < 0) {
		dev_info(rvu->dev, "Use echo <%s-lf > qsize\n", blk_string);
		goto qsize_write_done;
	}

	if (!rvu_dbg_is_valid_lf(rvu, blktype, lf, &pcifunc)) {
		ret = -EINVAL;
		goto qsize_write_done;
	}
	if (blktype  == BLKTYPE_NPA)
		rvu->rvu_dbg.npa_qsize_id = lf;
	else
		rvu->rvu_dbg.nix_qsize_id = lf;

qsize_write_done:
	kfree(cmd_buf_tmp);
	return ret ? ret : count;
}

static ssize_t rvu_dbg_npa_qsize_write(struct file *filp,
				       const char __user *buffer,
				       size_t count, loff_t *ppos)
{
	return rvu_dbg_qsize_write(filp, buffer, count, ppos,
					    BLKTYPE_NPA);
}

static int rvu_dbg_npa_qsize_display(struct seq_file *filp, void *unused)
{
	return rvu_dbg_qsize_display(filp, unused, BLKTYPE_NPA);
}

RVU_DEBUG_SEQ_FOPS(npa_qsize, npa_qsize_display, npa_qsize_write);

/* Dumps given NPA Aura's context */
static void print_npa_aura_ctx(struct seq_file *m, struct npa_aq_enq_rsp *rsp)
{
	struct npa_aura_s *aura = &rsp->aura;

	seq_printf(m, "W0: Pool addr\t\t%llx\n", aura->pool_addr);

	seq_printf(m, "W1: ena\t\t\t%d\nW1: pool caching\t%d\n",
		   aura->ena, aura->pool_caching);
	seq_printf(m, "W1: pool way mask\t%d\nW1: avg con\t\t%d\n",
		   aura->pool_way_mask, aura->avg_con);
	seq_printf(m, "W1: pool drop ena\t%d\nW1: aura drop ena\t%d\n",
		   aura->pool_drop_ena, aura->aura_drop_ena);
	seq_printf(m, "W1: bp_ena\t\t%d\nW1: aura drop\t\t%d\n",
		   aura->bp_ena, aura->aura_drop);
	seq_printf(m, "W1: aura shift\t\t%d\nW1: avg_level\t\t%d\n",
		   aura->shift, aura->avg_level);

	seq_printf(m, "W2: count\t\t%llu\nW2: nix0_bpid\t\t%d\nW2: nix1_bpid\t\t%d\n",
		   (u64)aura->count, aura->nix0_bpid, aura->nix1_bpid);

	seq_printf(m, "W3: limit\t\t%llu\nW3: bp\t\t\t%d\nW3: fc_ena\t\t%d\n",
		   (u64)aura->limit, aura->bp, aura->fc_ena);
	seq_printf(m, "W3: fc_up_crossing\t%d\nW3: fc_stype\t\t%d\n",
		   aura->fc_up_crossing, aura->fc_stype);
	seq_printf(m, "W3: fc_hyst_bits\t%d\n", aura->fc_hyst_bits);

	seq_printf(m, "W4: fc_addr\t\t%llx\n", aura->fc_addr);

	seq_printf(m, "W5: pool_drop\t\t%d\nW5: update_time\t\t%d\n",
		   aura->pool_drop, aura->update_time);
	seq_printf(m, "W5: err_int \t\t%d\nW5: err_int_ena\t\t%d\n",
		   aura->err_int, aura->err_int_ena);
	seq_printf(m, "W5: thresh_int\t\t%d\nW5: thresh_int_ena \t%d\n",
		   aura->thresh_int, aura->thresh_int_ena);
	seq_printf(m, "W5: thresh_up\t\t%d\nW5: thresh_qint_idx\t%d\n",
		   aura->thresh_up, aura->thresh_qint_idx);
	seq_printf(m, "W5: err_qint_idx \t%d\n", aura->err_qint_idx);

	seq_printf(m, "W6: thresh\t\t%llu\n", (u64)aura->thresh);
}

/* Dumps given NPA Pool's context */
static void print_npa_pool_ctx(struct seq_file *m, struct npa_aq_enq_rsp *rsp)
{
	struct npa_pool_s *pool = &rsp->pool;

	seq_printf(m, "W0: Stack base\t\t%llx\n", pool->stack_base);

	seq_printf(m, "W1: ena \t\t%d\nW1: nat_align \t\t%d\n",
		   pool->ena, pool->nat_align);
	seq_printf(m, "W1: stack_caching\t%d\nW1: stack_way_mask\t%d\n",
		   pool->stack_caching, pool->stack_way_mask);
	seq_printf(m, "W1: buf_offset\t\t%d\nW1: buf_size\t\t%d\n",
		   pool->buf_offset, pool->buf_size);

	seq_printf(m, "W2: stack_max_pages \t%d\nW2: stack_pages\t\t%d\n",
		   pool->stack_max_pages, pool->stack_pages);

	seq_printf(m, "W3: op_pc \t\t%llu\n", (u64)pool->op_pc);

	seq_printf(m, "W4: stack_offset\t%d\nW4: shift\t\t%d\nW4: avg_level\t\t%d\n",
		   pool->stack_offset, pool->shift, pool->avg_level);
	seq_printf(m, "W4: avg_con \t\t%d\nW4: fc_ena\t\t%d\nW4: fc_stype\t\t%d\n",
		   pool->avg_con, pool->fc_ena, pool->fc_stype);
	seq_printf(m, "W4: fc_hyst_bits\t%d\nW4: fc_up_crossing\t%d\n",
		   pool->fc_hyst_bits, pool->fc_up_crossing);
	seq_printf(m, "W4: update_time\t\t%d\n", pool->update_time);

	seq_printf(m, "W5: fc_addr\t\t%llx\n", pool->fc_addr);

	seq_printf(m, "W6: ptr_start\t\t%llx\n", pool->ptr_start);

	seq_printf(m, "W7: ptr_end\t\t%llx\n", pool->ptr_end);

	seq_printf(m, "W8: err_int\t\t%d\nW8: err_int_ena\t\t%d\n",
		   pool->err_int, pool->err_int_ena);
	seq_printf(m, "W8: thresh_int\t\t%d\n", pool->thresh_int);
	seq_printf(m, "W8: thresh_int_ena\t%d\nW8: thresh_up\t\t%d\n",
		   pool->thresh_int_ena, pool->thresh_up);
	seq_printf(m, "W8: thresh_qint_idx\t%d\nW8: err_qint_idx\t\t%d\n",
		   pool->thresh_qint_idx, pool->err_qint_idx);
}

/* Reads aura/pool's ctx from admin queue */
static int rvu_dbg_npa_ctx_display(struct seq_file *m, void *unused, int ctype)
{
	void (*print_npa_ctx)(struct seq_file *m, struct npa_aq_enq_rsp *rsp);
	struct npa_aq_enq_req aq_req;
	struct npa_aq_enq_rsp rsp;
	struct rvu_pfvf *pfvf;
	int aura, rc, max_id;
	int npalf, id, all;
	struct rvu *rvu;
	u16 pcifunc;

	rvu = m->private;

	switch (ctype) {
	case NPA_AQ_CTYPE_AURA:
		npalf = rvu->rvu_dbg.npa_aura_ctx.lf;
		id = rvu->rvu_dbg.npa_aura_ctx.id;
		all = rvu->rvu_dbg.npa_aura_ctx.all;
		break;

	case NPA_AQ_CTYPE_POOL:
		npalf = rvu->rvu_dbg.npa_pool_ctx.lf;
		id = rvu->rvu_dbg.npa_pool_ctx.id;
		all = rvu->rvu_dbg.npa_pool_ctx.all;
		break;

	default:
		return -EINVAL;
	}

	if (!rvu_dbg_is_valid_lf(rvu, BLKTYPE_NPA, npalf, &pcifunc))
		return -EINVAL;

	pfvf = rvu_get_pfvf(rvu, pcifunc);
	if (ctype == NPA_AQ_CTYPE_AURA && !pfvf->aura_ctx) {
		seq_puts(m, "Aura context is not initialized\n");
		return -EINVAL;
	} else if (ctype == NPA_AQ_CTYPE_POOL && !pfvf->pool_ctx) {
		seq_puts(m, "Pool context is not initialized\n");
		return -EINVAL;
	}

	memset(&aq_req, 0, sizeof(struct npa_aq_enq_req));
	aq_req.hdr.pcifunc = pcifunc;
	aq_req.ctype = ctype;
	aq_req.op = NPA_AQ_INSTOP_READ;
	if (ctype == NPA_AQ_CTYPE_AURA) {
		max_id = pfvf->aura_ctx->qsize;
		print_npa_ctx = print_npa_aura_ctx;
	} else {
		max_id = pfvf->pool_ctx->qsize;
		print_npa_ctx = print_npa_pool_ctx;
	}

	if (id < 0 || id >= max_id) {
		seq_printf(m, "Invalid %s, valid range is 0-%d\n",
			   (ctype == NPA_AQ_CTYPE_AURA) ? "aura" : "pool",
			max_id - 1);
		return -EINVAL;
	}

	if (all)
		id = 0;
	else
		max_id = id + 1;

	for (aura = id; aura < max_id; aura++) {
		aq_req.aura_id = aura;
		seq_printf(m, "======%s : %d=======\n",
			   (ctype == NPA_AQ_CTYPE_AURA) ? "AURA" : "POOL",
			aq_req.aura_id);
		rc = rvu_npa_aq_enq_inst(rvu, &aq_req, &rsp);
		if (rc) {
			seq_puts(m, "Failed to read context\n");
			return -EINVAL;
		}
		print_npa_ctx(m, &rsp);
	}
	return 0;
}

static int write_npa_ctx(struct rvu *rvu, bool all,
			 int npalf, int id, int ctype)
{
	struct rvu_pfvf *pfvf;
	int max_id;
	u16 pcifunc;

	if (!rvu_dbg_is_valid_lf(rvu, BLKTYPE_NPA, npalf, &pcifunc))
		return -EINVAL;

	pfvf = rvu_get_pfvf(rvu, pcifunc);

	if (ctype == NPA_AQ_CTYPE_AURA) {
		if (!pfvf->aura_ctx) {
			dev_warn(rvu->dev, "Aura context is not initialized\n");
			return -EINVAL;
		}
		max_id = pfvf->aura_ctx->qsize;
	} else if (ctype == NPA_AQ_CTYPE_POOL) {
		if (!pfvf->pool_ctx) {
			dev_warn(rvu->dev, "Pool context is not initialized\n");
			return -EINVAL;
		}
		max_id = pfvf->pool_ctx->qsize;
	}

	if (id < 0 || id >= max_id) {
		dev_warn(rvu->dev, "Invalid %s, valid range is 0-%d\n",
			 (ctype == NPA_AQ_CTYPE_AURA) ? "aura" : "pool",
			max_id - 1);
		return -EINVAL;
	}

	switch (ctype) {
	case NPA_AQ_CTYPE_AURA:
		rvu->rvu_dbg.npa_aura_ctx.lf = npalf;
		rvu->rvu_dbg.npa_aura_ctx.id = id;
		rvu->rvu_dbg.npa_aura_ctx.all = all;
		break;

	case NPA_AQ_CTYPE_POOL:
		rvu->rvu_dbg.npa_pool_ctx.lf = npalf;
		rvu->rvu_dbg.npa_pool_ctx.id = id;
		rvu->rvu_dbg.npa_pool_ctx.all = all;
		break;

	default:
		return -EINVAL;
	}
	return 0;
}

static int parse_cmd_buffer_ctx(char *cmd_buf, size_t *count,
				const char __user *buffer, int *npalf,
				int *id, bool *all)
{
	int bytes_not_copied;
	char *cmd_buf_tmp;
	char *subtoken;
	int ret;

	bytes_not_copied = copy_from_user(cmd_buf, buffer, *count);
	if (bytes_not_copied)
		return -EFAULT;

	cmd_buf[*count] = '\0';
	cmd_buf_tmp = strchr(cmd_buf, '\n');

	if (cmd_buf_tmp) {
		*cmd_buf_tmp = '\0';
		*count = cmd_buf_tmp - cmd_buf + 1;
	}

	subtoken = strsep(&cmd_buf, " ");
	ret = subtoken ? kstrtoint(subtoken, 10, npalf) : -EINVAL;
	if (ret < 0)
		return ret;
	subtoken = strsep(&cmd_buf, " ");
	if (subtoken && strcmp(subtoken, "all") == 0) {
		*all = true;
	} else {
		ret = subtoken ? kstrtoint(subtoken, 10, id) : -EINVAL;
		if (ret < 0)
			return ret;
	}
	if (cmd_buf)
		return -EINVAL;
	return ret;
}

static ssize_t rvu_dbg_npa_ctx_write(struct file *filp,
				     const char __user *buffer,
				     size_t count, loff_t *ppos, int ctype)
{
	char *cmd_buf, *ctype_string = (ctype == NPA_AQ_CTYPE_AURA) ?
					"aura" : "pool";
	struct seq_file *seqfp = filp->private_data;
	struct rvu *rvu = seqfp->private;
	int npalf, id = 0, ret;
	bool all = false;

	if ((*ppos != 0) || !count)
		return -EINVAL;

	cmd_buf = kzalloc(count + 1, GFP_KERNEL);
	if (!cmd_buf)
		return count;

	ret = parse_cmd_buffer_ctx(cmd_buf, &count, buffer,
				   &npalf, &id, &all);
	if (ret < 0) {
		dev_info(rvu->dev,
			 "Usage: echo <npalf> [%s number/all] > %s_ctx\n",
			 ctype_string, ctype_string);
		goto done;
	} else {
		ret = write_npa_ctx(rvu, all, npalf, id, ctype);
	}
done:
	kfree(cmd_buf);
	return ret ? ret : count;
}

static ssize_t rvu_dbg_npa_aura_ctx_write(struct file *filp,
					  const char __user *buffer,
					  size_t count, loff_t *ppos)
{
	return rvu_dbg_npa_ctx_write(filp, buffer, count, ppos,
				     NPA_AQ_CTYPE_AURA);
}

static int rvu_dbg_npa_aura_ctx_display(struct seq_file *filp, void *unused)
{
	return rvu_dbg_npa_ctx_display(filp, unused, NPA_AQ_CTYPE_AURA);
}

RVU_DEBUG_SEQ_FOPS(npa_aura_ctx, npa_aura_ctx_display, npa_aura_ctx_write);

static ssize_t rvu_dbg_npa_pool_ctx_write(struct file *filp,
					  const char __user *buffer,
					  size_t count, loff_t *ppos)
{
	return rvu_dbg_npa_ctx_write(filp, buffer, count, ppos,
				     NPA_AQ_CTYPE_POOL);
}

static int rvu_dbg_npa_pool_ctx_display(struct seq_file *filp, void *unused)
{
	return rvu_dbg_npa_ctx_display(filp, unused, NPA_AQ_CTYPE_POOL);
}

RVU_DEBUG_SEQ_FOPS(npa_pool_ctx, npa_pool_ctx_display, npa_pool_ctx_write);

static void ndc_cache_stats(struct seq_file *s, int blk_addr,
			    int ctype, int transaction)
{
	u64 req, out_req, lat, cant_alloc;
	struct rvu *rvu = s->private;
	int port;

	for (port = 0; port < NDC_MAX_PORT; port++) {
		req = rvu_read64(rvu, blk_addr, NDC_AF_PORTX_RTX_RWX_REQ_PC
						(port, ctype, transaction));
		lat = rvu_read64(rvu, blk_addr, NDC_AF_PORTX_RTX_RWX_LAT_PC
						(port, ctype, transaction));
		out_req = rvu_read64(rvu, blk_addr,
				     NDC_AF_PORTX_RTX_RWX_OSTDN_PC
				     (port, ctype, transaction));
		cant_alloc = rvu_read64(rvu, blk_addr,
					NDC_AF_PORTX_RTX_CANT_ALLOC_PC
					(port, transaction));
		seq_printf(s, "\nPort:%d\n", port);
		seq_printf(s, "\tTotal Requests:\t\t%lld\n", req);
		seq_printf(s, "\tTotal Time Taken:\t%lld cycles\n", lat);
		seq_printf(s, "\tAvg Latency:\t\t%lld cycles\n", lat / req);
		seq_printf(s, "\tOutstanding Requests:\t%lld\n", out_req);
		seq_printf(s, "\tCant Alloc Requests:\t%lld\n", cant_alloc);
	}
}

static int ndc_blk_cache_stats(struct seq_file *s, int idx, int blk_addr)
{
	seq_puts(s, "\n***** CACHE mode read stats *****\n");
	ndc_cache_stats(s, blk_addr, CACHING, NDC_READ_TRANS);
	seq_puts(s, "\n***** CACHE mode write stats *****\n");
	ndc_cache_stats(s, blk_addr, CACHING, NDC_WRITE_TRANS);
	seq_puts(s, "\n***** BY-PASS mode read stats *****\n");
	ndc_cache_stats(s, blk_addr, BYPASS, NDC_READ_TRANS);
	seq_puts(s, "\n***** BY-PASS mode write stats *****\n");
	ndc_cache_stats(s, blk_addr, BYPASS, NDC_WRITE_TRANS);
	return 0;
}

static int rvu_dbg_npa_ndc_cache_display(struct seq_file *filp, void *unused)
{
	return ndc_blk_cache_stats(filp, NPA0_U, BLKADDR_NDC_NPA0);
}

RVU_DEBUG_SEQ_FOPS(npa_ndc_cache, npa_ndc_cache_display, NULL);

static int ndc_blk_hits_miss_stats(struct seq_file *s, int idx, int blk_addr)
{
	struct rvu *rvu = s->private;
	int bank, max_bank;

	max_bank = NDC_MAX_BANK(rvu, blk_addr);
	for (bank = 0; bank < max_bank; bank++) {
		seq_printf(s, "BANK:%d\n", bank);
		seq_printf(s, "\tHits:\t%lld\n",
			   (u64)rvu_read64(rvu, blk_addr,
			   NDC_AF_BANKX_HIT_PC(bank)));
		seq_printf(s, "\tMiss:\t%lld\n",
			   (u64)rvu_read64(rvu, blk_addr,
			    NDC_AF_BANKX_MISS_PC(bank)));
	}
	return 0;
}

static int rvu_dbg_npa_ndc_hits_miss_display(struct seq_file *filp,
					     void *unused)
{
	return ndc_blk_hits_miss_stats(filp, NPA0_U, BLKADDR_NDC_NPA0);
}

RVU_DEBUG_SEQ_FOPS(npa_ndc_hits_miss, npa_ndc_hits_miss_display, NULL);

static void rvu_dbg_npa_init(struct rvu *rvu)
{
	const struct device *dev = &rvu->pdev->dev;
	struct dentry *pfile;

	rvu->rvu_dbg.npa = debugfs_create_dir("npa", rvu->rvu_dbg.root);
	if (!rvu->rvu_dbg.npa)
		return;

	pfile = debugfs_create_file("qsize", 0600, rvu->rvu_dbg.npa, rvu,
				    &rvu_dbg_npa_qsize_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("aura_ctx", 0600, rvu->rvu_dbg.npa, rvu,
				    &rvu_dbg_npa_aura_ctx_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("pool_ctx", 0600, rvu->rvu_dbg.npa, rvu,
				    &rvu_dbg_npa_pool_ctx_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("ndc_cache", 0600, rvu->rvu_dbg.npa, rvu,
				    &rvu_dbg_npa_ndc_cache_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("ndc_hits_miss", 0600, rvu->rvu_dbg.npa,
				    rvu, &rvu_dbg_npa_ndc_hits_miss_fops);
	if (!pfile)
		goto create_failed;

	return;
create_failed:
	dev_err(dev, "Failed to create debugfs dir/file for NPA\n");
	debugfs_remove_recursive(rvu->rvu_dbg.npa);
}

static int cgx_print_stats(struct seq_file *s, int lmac_id)
{
	struct cgx_link_user_info linfo;
	void *cgxd = s->private;
	int stat = 0, err = 0;
	u64 tx_stat, rx_stat;

	/* Link status */
	seq_puts(s, "\n=======Link Status======\n\n");
	err = cgx_get_link_info(cgxd, lmac_id, &linfo);
	if (err)
		seq_puts(s, "Failed to read link status\n");
	seq_printf(s, "\nLink is %s %d Mbps\n\n",
		   linfo.link_up ? "UP" : "DOWN", linfo.speed);

	/* Rx stats */
	seq_puts(s, "\n=======RX_STATS======\n\n");
	while (stat < CGX_RX_STATS_COUNT) {
		err = cgx_get_rx_stats(cgxd, lmac_id, stat, &rx_stat);
		if (err)
			return err;
		seq_printf(s, "%s: %llu\n", cgx_rx_stats_fields[stat], rx_stat);
		stat++;
	}

	/* Tx stats */
	stat = 0;
	seq_puts(s, "\n=======TX_STATS======\n\n");
	while (stat < CGX_TX_STATS_COUNT) {
		err = cgx_get_tx_stats(cgxd, lmac_id, stat, &tx_stat);
		if (err)
			return err;
		seq_printf(s, "%s: %llu\n", cgx_tx_stats_fields[stat], tx_stat);
		stat++;
	}
	return err;
}

static int rvu_dbg_cgx_stat_display(struct seq_file *filp, void *unused)
{
	struct dentry *current_dir;
	int err, lmac_id;
	char *buf;

	current_dir = filp->file->f_path.dentry->d_parent;
	buf = strrchr(current_dir->d_name.name, 'c');
	if (!buf)
		return -EINVAL;

	err = kstrtoint(buf + 1, 10, &lmac_id);
	if (!err) {
		err = cgx_print_stats(filp, lmac_id);
		if (err)
			return err;
	}
	return err;
}

RVU_DEBUG_SEQ_FOPS(cgx_stat, cgx_stat_display, NULL);

static void rvu_dbg_cgx_init(struct rvu *rvu)
{
	const struct device *dev = &rvu->pdev->dev;
	struct dentry *pfile;
	int i, lmac_id;
	char dname[20];
	void *cgx;

	rvu->rvu_dbg.cgx_root = debugfs_create_dir("cgx", rvu->rvu_dbg.root);

	for (i = 0; i < cgx_get_cgxcnt_max(); i++) {
		cgx = rvu_cgx_pdata(i, rvu);
		if (!cgx)
			continue;
		/* cgx debugfs dir */
		sprintf(dname, "cgx%d", i);
		rvu->rvu_dbg.cgx = debugfs_create_dir(dname,
						      rvu->rvu_dbg.cgx_root);
		for (lmac_id = 0; lmac_id < cgx_get_lmac_cnt(cgx); lmac_id++) {
			/* lmac debugfs dir */
			sprintf(dname, "lmac%d", lmac_id);
			rvu->rvu_dbg.lmac =
				debugfs_create_dir(dname, rvu->rvu_dbg.cgx);

			pfile =	debugfs_create_file("stats", 0600,
						    rvu->rvu_dbg.lmac, cgx,
						    &rvu_dbg_cgx_stat_fops);
			if (!pfile)
				goto create_failed;
		}
	}
	return;

create_failed:
	dev_err(dev, "Failed to create debugfs dir/file for CGX\n");
	debugfs_remove_recursive(rvu->rvu_dbg.cgx_root);
}

/* Dumps given nix_sq's context */
static void print_nix_sq_ctx(struct seq_file *m, struct nix_aq_enq_rsp *rsp)
{
	struct nix_sq_ctx_s *sq_ctx = &rsp->sq;

	seq_printf(m, "W0: sqe_way_mask \t\t%d\nW0: cq \t\t\t\t%d\n",
		   sq_ctx->sqe_way_mask, sq_ctx->cq);
	seq_printf(m, "W0: sdp_mcast \t\t\t%d\nW0: substream \t\t\t0x%03x\n",
		   sq_ctx->sdp_mcast, sq_ctx->substream);
	seq_printf(m, "W0: qint_idx \t\t%d\nW0: ena \t\t\t\t%d\n\n",
		   sq_ctx->qint_idx, sq_ctx->ena);

	seq_printf(m, "W1: sqb_count \t\t\t%d\nW1: default_chan \t\t%d\n",
		   sq_ctx->sqb_count, sq_ctx->default_chan);
	seq_printf(m, "W1: smq_rr_quantum \t\t%d\nW1: sso_ena \t\t\t%d\n",
		   sq_ctx->smq_rr_quantum, sq_ctx->sso_ena);
	seq_printf(m, "W1: xoff \t\t\t%d\nW1: cq_ena \t\t\t%d\nW1: smq\t\t\t\t%d\n\n",
		   sq_ctx->xoff, sq_ctx->cq_ena, sq_ctx->smq);

	seq_printf(m, "W2: sqe_stype \t\t\t%d\nW2: sq_int_ena \t\t\t%d\n",
		   sq_ctx->sqe_stype, sq_ctx->sq_int_ena);
	seq_printf(m, "W2: sq_int \t\t\t%d\nW2: sqb_aura \t\t\t%d\n",
		   sq_ctx->sq_int, sq_ctx->sqb_aura);
	seq_printf(m, "W2: smq_rr_count \t\t%d\n\n", sq_ctx->smq_rr_count);

	seq_printf(m, "W3: smq_next_sq_vld\t\t%d\nW3: smq_pend\t\t\t%d\n",
		   sq_ctx->smq_next_sq_vld, sq_ctx->smq_pend);
	seq_printf(m, "W3: smenq_next_sqb_vld \t\t%d\nW3: head_offset\t\t\t%d\n",
		   sq_ctx->smenq_next_sqb_vld, sq_ctx->head_offset);
	seq_printf(m, "W3: smenq_offset\t\t%d\nW3: tail_offset\t\t%d\n",
		   sq_ctx->smenq_offset, sq_ctx->tail_offset);
	seq_printf(m, "W3: smq_lso_segnum \t\t%d\nW3: smq_next_sq\t\t\t%d\n",
		   sq_ctx->smq_lso_segnum, sq_ctx->smq_next_sq);
	seq_printf(m, "W3: mnq_dis \t\t\t%d\nW3: lmt_dis \t\t\t%d\n",
		   sq_ctx->mnq_dis, sq_ctx->lmt_dis);
	seq_printf(m, "W3: cq_limit\t\t\t%d\nW3: max_sqe_size\t\t%d\n\n",
		   sq_ctx->cq_limit, sq_ctx->max_sqe_size);

	seq_printf(m, "W4: next_sqb \t\t\t%llx\n\n", sq_ctx->next_sqb);
	seq_printf(m, "W5: tail_sqb \t\t\t%llx\n\n", sq_ctx->tail_sqb);
	seq_printf(m, "W6: smenq_sqb \t\t\t%llx\n\n", sq_ctx->smenq_sqb);
	seq_printf(m, "W7: smenq_next_sqb \t\t%llx\n\n",
		   sq_ctx->smenq_next_sqb);

	seq_printf(m, "W8: head_sqb\t\t\t%llx\n\n", sq_ctx->head_sqb);

	seq_printf(m, "W9: vfi_lso_vld\t\t\t%d\nW9: vfi_lso_vlan1_ins_ena\t%d\n",
		   sq_ctx->vfi_lso_vld, sq_ctx->vfi_lso_vlan1_ins_ena);
	seq_printf(m, "W9: vfi_lso_vlan0_ins_ena\t%d\nW9: vfi_lso_mps\t\t\t%d\n",
		   sq_ctx->vfi_lso_vlan0_ins_ena, sq_ctx->vfi_lso_mps);
	seq_printf(m, "W9: vfi_lso_sb\t\t\t%d\nW9: vfi_lso_sizem1\t\t%d\n",
		   sq_ctx->vfi_lso_sb, sq_ctx->vfi_lso_sizem1);
	seq_printf(m, "W9: vfi_lso_total\t\t%d\n\n", sq_ctx->vfi_lso_total);

	seq_printf(m, "W10: scm_lso_rem \t\t%llu\n\n",
		   (u64)sq_ctx->scm_lso_rem);
	seq_printf(m, "W11: octs \t\t\t%llu\n\n", (u64)sq_ctx->octs);
	seq_printf(m, "W12: pkts \t\t\t%llu\n\n", (u64)sq_ctx->pkts);
	seq_printf(m, "W14: dropped_octs \t\t%llu\n\n",
		   (u64)sq_ctx->dropped_octs);
	seq_printf(m, "W15: dropped_pkts \t\t%llu\n\n",
		   (u64)sq_ctx->dropped_pkts);
}

/* Dumps given nix_rq's context */
static void print_nix_rq_ctx(struct seq_file *m, struct nix_aq_enq_rsp *rsp)
{
	struct  nix_rq_ctx_s *rq_ctx = &rsp->rq;

	seq_printf(m, "W0: wqe_aura \t\t\t%d\nW0: substream \t\t\t0x%03x\n",
		   rq_ctx->wqe_aura, rq_ctx->substream);
	seq_printf(m, "W0: cq \t\t\t\t%d\nW0: ena_wqwd \t\t\t%d\n",
		   rq_ctx->cq, rq_ctx->ena_wqwd);
	seq_printf(m, "W0: ipsech_ena \t\t\t%d\nW0: sso_ena \t\t\t%d\n",
		   rq_ctx->ipsech_ena, rq_ctx->sso_ena);
	seq_printf(m, "W0: ena \t\t\t\t%d\n\n", rq_ctx->ena);

	seq_printf(m, "W1: lpb_drop_ena \t\t%d\nW1: spb_drop_ena \t\t%d\n",
		   rq_ctx->lpb_drop_ena, rq_ctx->spb_drop_ena);
	seq_printf(m, "W1: xqe_drop_ena \t\t%d\nW1: wqe_caching \t\t\t%d\n",
		   rq_ctx->xqe_drop_ena, rq_ctx->wqe_caching);
	seq_printf(m, "W1: pb_caching \t\t\t%d\nW1: sso_tt \t\t\t%d\n",
		   rq_ctx->pb_caching, rq_ctx->sso_tt);
	seq_printf(m, "W1: sso_grp \t\t\t%d\nW1: lpb_aura \t\t\t%d\n",
		   rq_ctx->sso_grp, rq_ctx->lpb_aura);
	seq_printf(m, "W1: spb_aura \t\t\t%d\n\n", rq_ctx->spb_aura);

	seq_printf(m, "W2: xqe_hdr_split \t\t%d\nW2: xqe_imm_copy \t\t%d\n",
		   rq_ctx->xqe_hdr_split, rq_ctx->xqe_imm_copy);
	seq_printf(m, "W2: xqe_imm_size \t\t%d\nW2: later_skip \t\t\t%d\n",
		   rq_ctx->xqe_imm_size, rq_ctx->later_skip);
	seq_printf(m, "W2: first_skip \t\t\t%d\nW2: lpb_sizem1 \t\t\t%d\n",
		   rq_ctx->first_skip, rq_ctx->lpb_sizem1);
	seq_printf(m, "W2: spb_ena \t\t\t%d\nW2: wqe_skip \t\t\t%d\n",
		   rq_ctx->spb_ena, rq_ctx->wqe_skip);
	seq_printf(m, "W2: spb_sizem1 \t\t\t%d\n\n", rq_ctx->spb_sizem1);

	seq_printf(m, "W3: spb_pool_pass \t\t%d\nW3: spb_pool_drop \t\t%d\n",
		   rq_ctx->spb_pool_pass, rq_ctx->spb_pool_drop);
	seq_printf(m, "W3: spb_aura_pass \t\t%d\nW3: spb_aura_drop \t\t%d\n",
		   rq_ctx->spb_aura_pass, rq_ctx->spb_aura_drop);
	seq_printf(m, "W3: wqe_pool_pass \t\t%d\nW3: wqe_pool_drop \t\t%d\n",
		   rq_ctx->wqe_pool_pass, rq_ctx->wqe_pool_drop);
	seq_printf(m, "W3: xqe_pass \t\t\t%d\nW3: xqe_drop \t\t\t%d\n\n",
		   rq_ctx->xqe_pass, rq_ctx->xqe_drop);

	seq_printf(m, "W4: qint_idx \t\t\t%d\nW4: rq_int_ena \t\t\t%d\n",
		   rq_ctx->qint_idx, rq_ctx->rq_int_ena);
	seq_printf(m, "W4: rq_int \t\t\t%d\nW4: lpb_pool_pass \t\t%d\n",
		   rq_ctx->rq_int, rq_ctx->lpb_pool_pass);
	seq_printf(m, "W4: lpb_pool_drop \t\t%d\nW4: lpb_aura_pass \t\t%d\n",
		   rq_ctx->lpb_pool_drop, rq_ctx->lpb_aura_pass);
	seq_printf(m, "W4: lpb_aura_drop \t\t%d\n\n", rq_ctx->lpb_aura_drop);

	seq_printf(m, "W5: flow_tagw \t\t\t%d\nW5: bad_utag \t\t\t%d\n",
		   rq_ctx->flow_tagw, rq_ctx->bad_utag);
	seq_printf(m, "W5: good_utag \t\t\t%d\nW5: ltag \t\t\t%d\n\n",
		   rq_ctx->good_utag, rq_ctx->ltag);

	seq_printf(m, "W6: octs \t\t\t%llu\n\n", (u64)rq_ctx->octs);
	seq_printf(m, "W7: pkts \t\t\t%llu\n\n", (u64)rq_ctx->pkts);
	seq_printf(m, "W8: drop_octs \t\t\t%llu\n\n", (u64)rq_ctx->drop_octs);
	seq_printf(m, "W9: drop_pkts \t\t\t%llu\n\n", (u64)rq_ctx->drop_pkts);
	seq_printf(m, "W10: re_pkts \t\t\t%llu\n", (u64)rq_ctx->re_pkts);
}

/* Dumps given nix_cq's context */
static void print_nix_cq_ctx(struct seq_file *m, struct nix_aq_enq_rsp *rsp)
{
	struct  nix_cq_ctx_s *cq_ctx = &rsp->cq;

	seq_printf(m, "W0: base \t\t\t%llx\n\n", cq_ctx->base);

	seq_printf(m, "W1: wrptr \t\t\t%llx\n", (u64)cq_ctx->wrptr);
	seq_printf(m, "W1: avg_con \t\t\t%d\nW1: cint_idx \t\t\t%d\n",
		   cq_ctx->avg_con, cq_ctx->cint_idx);
	seq_printf(m, "W1: cq_err \t\t\t%d\nW1: qint_idx \t\t\t%d\n",
		   cq_ctx->cq_err, cq_ctx->qint_idx);
	seq_printf(m, "W1: bpid \t\t\t%d\nW1: bp_ena \t\t\t%d\n\n",
		   cq_ctx->bpid, cq_ctx->bp_ena);

	seq_printf(m, "W2: update_time \t\t\t%d\nW2:avg_level \t\t\t%d\n",
		   cq_ctx->update_time, cq_ctx->avg_level);
	seq_printf(m, "W2: head \t\t\t%d\nW2:tail \t\t\t\t%d\n\n",
		   cq_ctx->head, cq_ctx->tail);

	seq_printf(m, "W3: cq_err_int_ena \t\t%d\nW3:cq_err_int \t\t\t%d\n",
		   cq_ctx->cq_err_int_ena, cq_ctx->cq_err_int);
	seq_printf(m, "W3: qsize \t\t\t%d\nW3:caching \t\t\t%d\n",
		   cq_ctx->qsize, cq_ctx->caching);
	seq_printf(m, "W3: substream \t\t\t0x%03x\nW3: ena \t\t\t\t%d\n",
		   cq_ctx->substream, cq_ctx->ena);
	seq_printf(m, "W3: drop_ena \t\t\t%d\nW3: drop \t\t\t%d\n",
		   cq_ctx->drop_ena, cq_ctx->drop);
	seq_printf(m, "W3: bp \t\t\t\t%d\n\n", cq_ctx->bp);
}

static int rvu_dbg_nix_queue_ctx_display(struct seq_file *filp,
					 void *unused, int ctype)
{
	void (*print_nix_ctx)(struct seq_file *filp,
			      struct nix_aq_enq_rsp *rsp) = NULL;
	struct rvu *rvu = filp->private;
	struct nix_aq_enq_req aq_req;
	struct nix_aq_enq_rsp rsp;
	char *ctype_string = NULL;
	int qidx, rc, max_id = 0;
	struct rvu_pfvf *pfvf;
	int nixlf, id, all;
	u16 pcifunc;

	switch (ctype) {
	case NIX_AQ_CTYPE_CQ:
		nixlf = rvu->rvu_dbg.nix_cq_ctx.lf;
		id = rvu->rvu_dbg.nix_cq_ctx.id;
		all = rvu->rvu_dbg.nix_cq_ctx.all;
		break;

	case NIX_AQ_CTYPE_SQ:
		nixlf = rvu->rvu_dbg.nix_sq_ctx.lf;
		id = rvu->rvu_dbg.nix_sq_ctx.id;
		all = rvu->rvu_dbg.nix_sq_ctx.all;
		break;

	case NIX_AQ_CTYPE_RQ:
		nixlf = rvu->rvu_dbg.nix_rq_ctx.lf;
		id = rvu->rvu_dbg.nix_rq_ctx.id;
		all = rvu->rvu_dbg.nix_rq_ctx.all;
		break;

	default:
		return -EINVAL;
	}

	if (!rvu_dbg_is_valid_lf(rvu, BLKTYPE_NIX, nixlf, &pcifunc))
		return -EINVAL;

	pfvf = rvu_get_pfvf(rvu, pcifunc);
	if (ctype == NIX_AQ_CTYPE_SQ && !pfvf->sq_ctx) {
		seq_puts(filp, "SQ context is not initialized\n");
		return -EINVAL;
	} else if (ctype == NIX_AQ_CTYPE_RQ && !pfvf->rq_ctx) {
		seq_puts(filp, "RQ context is not initialized\n");
		return -EINVAL;
	} else if (ctype == NIX_AQ_CTYPE_CQ && !pfvf->cq_ctx) {
		seq_puts(filp, "CQ context is not initialized\n");
		return -EINVAL;
	}

	if (ctype == NIX_AQ_CTYPE_SQ) {
		max_id = pfvf->sq_ctx->qsize;
		ctype_string = "sq";
		print_nix_ctx = print_nix_sq_ctx;
	} else if (ctype == NIX_AQ_CTYPE_RQ) {
		max_id = pfvf->rq_ctx->qsize;
		ctype_string = "rq";
		print_nix_ctx = print_nix_rq_ctx;
	} else if (ctype == NIX_AQ_CTYPE_CQ) {
		max_id = pfvf->cq_ctx->qsize;
		ctype_string = "cq";
		print_nix_ctx = print_nix_cq_ctx;
	}

	memset(&aq_req, 0, sizeof(struct nix_aq_enq_req));
	aq_req.hdr.pcifunc = pcifunc;
	aq_req.ctype = ctype;
	aq_req.op = NIX_AQ_INSTOP_READ;
	if (all)
		id = 0;
	else
		max_id = id + 1;
	for (qidx = id; qidx < max_id; qidx++) {
		aq_req.qidx = qidx;
		seq_printf(filp, "=====%s_ctx for nixlf:%d and qidx:%d is=====\n",
			   ctype_string, nixlf, aq_req.qidx);
		rc = rvu_mbox_handler_nix_aq_enq(rvu, &aq_req, &rsp);
		if (rc) {
			seq_puts(filp, "Failed to read the context\n");
			return -EINVAL;
		}
		print_nix_ctx(filp, &rsp);
	}
	return 0;
}

static int write_nix_queue_ctx(struct rvu *rvu, bool all, int nixlf,
			       int id, int ctype, char *ctype_string)
{
	struct rvu_pfvf *pfvf;
	int max_id = 0;
	u16 pcifunc;

	if (!rvu_dbg_is_valid_lf(rvu, BLKTYPE_NIX, nixlf, &pcifunc))
		return -EINVAL;

	pfvf = rvu_get_pfvf(rvu, pcifunc);

	if (ctype == NIX_AQ_CTYPE_SQ) {
		if (!pfvf->sq_ctx) {
			dev_warn(rvu->dev, "SQ context is not initialized\n");
			return -EINVAL;
		}
		max_id = pfvf->sq_ctx->qsize;
	} else if (ctype == NIX_AQ_CTYPE_RQ) {
		if (!pfvf->rq_ctx) {
			dev_warn(rvu->dev, "RQ context is not initialized\n");
			return -EINVAL;
		}
		max_id = pfvf->rq_ctx->qsize;
	} else if (ctype == NIX_AQ_CTYPE_CQ) {
		if (!pfvf->cq_ctx) {
			dev_warn(rvu->dev, "CQ context is not initialized\n");
			return -EINVAL;
		}
		max_id = pfvf->cq_ctx->qsize;
	}

	if (id < 0 || id >= max_id) {
		dev_warn(rvu->dev, "Invalid %s_ctx valid range 0-%d\n",
			 ctype_string, max_id - 1);
		return -EINVAL;
	}
	switch (ctype) {
	case NIX_AQ_CTYPE_CQ:
		rvu->rvu_dbg.nix_cq_ctx.lf = nixlf;
		rvu->rvu_dbg.nix_cq_ctx.id = id;
		rvu->rvu_dbg.nix_cq_ctx.all = all;
		break;

	case NIX_AQ_CTYPE_SQ:
		rvu->rvu_dbg.nix_sq_ctx.lf = nixlf;
		rvu->rvu_dbg.nix_sq_ctx.id = id;
		rvu->rvu_dbg.nix_sq_ctx.all = all;
		break;

	case NIX_AQ_CTYPE_RQ:
		rvu->rvu_dbg.nix_rq_ctx.lf = nixlf;
		rvu->rvu_dbg.nix_rq_ctx.id = id;
		rvu->rvu_dbg.nix_rq_ctx.all = all;
		break;

	default:
		return -EINVAL;
	}
	return 0;
}

static ssize_t rvu_dbg_nix_queue_ctx_write(struct file *filp,
					   const char __user *buffer,
					   size_t count, loff_t *ppos,
					   int ctype)
{
	struct seq_file *m = filp->private_data;
	struct rvu *rvu = m->private;
	char *cmd_buf, *ctype_string;
	int nixlf, id = 0, ret;
	bool all = false;

	if ((*ppos != 0) || !count)
		return -EINVAL;

	switch (ctype) {
	case NIX_AQ_CTYPE_SQ:
		ctype_string = "sq";
		break;
	case NIX_AQ_CTYPE_RQ:
		ctype_string = "rq";
		break;
	case NIX_AQ_CTYPE_CQ:
		ctype_string = "cq";
		break;
	default:
		return -EINVAL;
	}

	cmd_buf = kzalloc(count + 1, GFP_KERNEL);

	if (!cmd_buf)
		return count;

	ret = parse_cmd_buffer_ctx(cmd_buf, &count, buffer,
				   &nixlf, &id, &all);
	if (ret < 0) {
		dev_info(rvu->dev, "Usage: echo <nixlf> [%s number/all] > %s_ctx\n",
			 ctype_string, ctype_string);
		goto done;
	} else {
		ret = write_nix_queue_ctx(rvu, all, nixlf, id, ctype,
					  ctype_string);
	}
done:
	kfree(cmd_buf);
	return ret ? ret : count;
}

static ssize_t rvu_dbg_nix_sq_ctx_write(struct file *filp,
					const char __user *buffer,
					size_t count, loff_t *ppos)
{
	return rvu_dbg_nix_queue_ctx_write(filp, buffer, count, ppos,
					    NIX_AQ_CTYPE_SQ);
}

static int rvu_dbg_nix_sq_ctx_display(struct seq_file *filp, void *unused)
{
	return rvu_dbg_nix_queue_ctx_display(filp, unused, NIX_AQ_CTYPE_SQ);
}

RVU_DEBUG_SEQ_FOPS(nix_sq_ctx, nix_sq_ctx_display, nix_sq_ctx_write);

static ssize_t rvu_dbg_nix_rq_ctx_write(struct file *filp,
					const char __user *buffer,
					size_t count, loff_t *ppos)
{
	return rvu_dbg_nix_queue_ctx_write(filp, buffer, count, ppos,
					    NIX_AQ_CTYPE_RQ);
}

static int rvu_dbg_nix_rq_ctx_display(struct seq_file *filp, void  *unused)
{
	return rvu_dbg_nix_queue_ctx_display(filp, unused,  NIX_AQ_CTYPE_RQ);
}

RVU_DEBUG_SEQ_FOPS(nix_rq_ctx, nix_rq_ctx_display, nix_rq_ctx_write);

static ssize_t rvu_dbg_nix_cq_ctx_write(struct file *filp,
					const char __user *buffer,
					size_t count, loff_t *ppos)
{
	return rvu_dbg_nix_queue_ctx_write(filp, buffer, count, ppos,
					    NIX_AQ_CTYPE_CQ);
}

static int rvu_dbg_nix_cq_ctx_display(struct seq_file *filp, void *unused)
{
	return rvu_dbg_nix_queue_ctx_display(filp, unused, NIX_AQ_CTYPE_CQ);
}

RVU_DEBUG_SEQ_FOPS(nix_cq_ctx, nix_cq_ctx_display, nix_cq_ctx_write);

static int rvu_dbg_nix_ndc_rx_cache_display(struct seq_file *filp, void *unused)
{
	return ndc_blk_cache_stats(filp, NIX0_RX,
				   BLKADDR_NDC_NIX0_RX);
}

RVU_DEBUG_SEQ_FOPS(nix_ndc_rx_cache, nix_ndc_rx_cache_display, NULL);

static int rvu_dbg_nix_ndc_tx_cache_display(struct seq_file *filp, void *unused)
{
	return ndc_blk_cache_stats(filp, NIX0_TX,
				   BLKADDR_NDC_NIX0_TX);
}

RVU_DEBUG_SEQ_FOPS(nix_ndc_tx_cache, nix_ndc_tx_cache_display, NULL);

static int rvu_dbg_nix_ndc_rx_hits_miss_display(struct seq_file *filp,
						void *unused)
{
	return ndc_blk_hits_miss_stats(filp,
				      NPA0_U, BLKADDR_NDC_NIX0_RX);
}

RVU_DEBUG_SEQ_FOPS(nix_ndc_rx_hits_miss, nix_ndc_rx_hits_miss_display, NULL);

static int rvu_dbg_nix_ndc_tx_hits_miss_display(struct seq_file *filp,
						void *unused)
{
	return ndc_blk_hits_miss_stats(filp,
				      NPA0_U, BLKADDR_NDC_NIX0_TX);
}

RVU_DEBUG_SEQ_FOPS(nix_ndc_tx_hits_miss, nix_ndc_tx_hits_miss_display, NULL);

static ssize_t rvu_dbg_nix_tx_stall_hwissue_display(struct file *filp,
						    char __user *buffer,
						    size_t count, loff_t *ppos)
{
	return rvu_nix_get_tx_stall_counters(filp->private_data, buffer, ppos);
}

RVU_DEBUG_FOPS(nix_tx_stall_hwissue, nix_tx_stall_hwissue_display, NULL);

static void print_nix_qctx_qsize(struct seq_file *filp, int qsize,
				 unsigned long *bmap, char *qtype)
{
	char *buf;

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return;

	bitmap_print_to_pagebuf(false, buf, bmap, qsize);
	seq_printf(filp, "%s context count : %d\n", qtype, qsize);
	seq_printf(filp, "%s context ena/dis bitmap : %s\n",
		   qtype, buf);
	kfree(buf);
}

static void print_nix_qsize(struct seq_file *filp, struct rvu_pfvf *pfvf)
{
	if (!pfvf->cq_ctx)
		seq_puts(filp, "cq context is not initialized\n");
	else
		print_nix_qctx_qsize(filp, pfvf->cq_ctx->qsize, pfvf->cq_bmap,
				     "cq");

	if (!pfvf->rq_ctx)
		seq_puts(filp, "rq context is not initialized\n");
	else
		print_nix_qctx_qsize(filp, pfvf->rq_ctx->qsize, pfvf->rq_bmap,
				     "rq");

	if (!pfvf->sq_ctx)
		seq_puts(filp, "sq context is not initialized\n");
	else
		print_nix_qctx_qsize(filp, pfvf->sq_ctx->qsize, pfvf->sq_bmap,
				     "sq");
}

static ssize_t rvu_dbg_nix_qsize_write(struct file *filp,
				       const char __user *buffer,
				       size_t count, loff_t *ppos)
{
	return rvu_dbg_qsize_write(filp, buffer, count, ppos,
				   BLKTYPE_NIX);
}

static int rvu_dbg_nix_qsize_display(struct seq_file *filp, void *unused)
{
	return rvu_dbg_qsize_display(filp, unused, BLKTYPE_NIX);
}

RVU_DEBUG_SEQ_FOPS(nix_qsize, nix_qsize_display, nix_qsize_write);

static void rvu_dbg_nix_init(struct rvu *rvu)
{
	const struct device *dev = &rvu->pdev->dev;
	struct dentry *pfile;

	rvu->rvu_dbg.nix = debugfs_create_dir("nix", rvu->rvu_dbg.root);
	if (!rvu->rvu_dbg.nix) {
		dev_err(rvu->dev, "create debugfs dir failed for nix\n");
		return;
	}

	pfile = debugfs_create_file("sq_ctx", 0600, rvu->rvu_dbg.nix, rvu,
				    &rvu_dbg_nix_sq_ctx_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("rq_ctx", 0600, rvu->rvu_dbg.nix, rvu,
				    &rvu_dbg_nix_rq_ctx_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("cq_ctx", 0600, rvu->rvu_dbg.nix, rvu,
				    &rvu_dbg_nix_cq_ctx_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("ndc_tx_cache", 0600, rvu->rvu_dbg.nix, rvu,
				    &rvu_dbg_nix_ndc_tx_cache_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("ndc_rx_cache", 0600, rvu->rvu_dbg.nix, rvu,
				    &rvu_dbg_nix_ndc_rx_cache_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("ndc_tx_hits_miss", 0600, rvu->rvu_dbg.nix,
				    rvu, &rvu_dbg_nix_ndc_tx_hits_miss_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("ndc_rx_hits_miss", 0600, rvu->rvu_dbg.nix,
				    rvu, &rvu_dbg_nix_ndc_rx_hits_miss_fops);
	if (!pfile)
		goto create_failed;

	if (is_rvu_96xx_A0(rvu)) {
		pfile = debugfs_create_file("tx_stall_hwissue", 0600,
					    rvu->rvu_dbg.nix, rvu,
					    &rvu_dbg_nix_tx_stall_hwissue_fops);
		if (!pfile)
			goto create_failed;
	}

	pfile = debugfs_create_file("qsize", 0600, rvu->rvu_dbg.nix, rvu,
				    &rvu_dbg_nix_qsize_fops);
	if (!pfile)
		goto create_failed;

	return;
create_failed:
	dev_err(dev, "Failed to create debugfs dir/file for NIX\n");
	debugfs_remove_recursive(rvu->rvu_dbg.nix);
}

/* NPC debugfs APIs */
static inline void rvu_print_npc_mcam_info(struct seq_file *s,
					   u16 pcifunc, int blkaddr)
{
	struct rvu *rvu = s->private;
	int entry_acnt, entry_ecnt;
	int cntr_acnt, cntr_ecnt;

	/* Skip PF0 */
	if (!pcifunc)
		return;
	rvu_npc_get_mcam_entry_alloc_info(rvu, pcifunc, blkaddr,
					  &entry_acnt, &entry_ecnt);
	rvu_npc_get_mcam_counter_alloc_info(rvu, pcifunc, blkaddr,
					    &cntr_acnt, &cntr_ecnt);
	if (!entry_acnt && !cntr_acnt)
		return;

	if (!(pcifunc & RVU_PFVF_FUNC_MASK))
		seq_printf(s, "\n\t\t Device \t\t: PF%d\n",
			   rvu_get_pf(pcifunc));
	else
		seq_printf(s, "\n\t\t Device \t\t: PF%d VF%d\n",
			   rvu_get_pf(pcifunc),
			   (pcifunc & RVU_PFVF_FUNC_MASK) - 1);

	if (entry_acnt) {
		seq_printf(s, "\t\t Entries allocated \t: %d\n", entry_acnt);
		seq_printf(s, "\t\t Entries enabled \t: %d\n", entry_ecnt);
	}
	if (cntr_acnt) {
		seq_printf(s, "\t\t Counters allocated \t: %d\n", cntr_acnt);
		seq_printf(s, "\t\t Counters enabled \t: %d\n", cntr_ecnt);
	}
}

static int rvu_dbg_npc_mcam_info_display(struct seq_file *filp, void *unsued)
{
	struct rvu *rvu = filp->private;
	int pf, vf, numvfs, blkaddr;
	struct npc_mcam *mcam;
	u16 pcifunc;
	u64 cfg;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_NPC, 0);
	if (blkaddr < 0)
		return -ENODEV;

	mcam = &rvu->hw->mcam;

	seq_puts(filp, "\nNPC MCAM info:\n");
	/* MCAM keywidth on receive and transmit sides */
	cfg = rvu_read64(rvu, blkaddr, NPC_AF_INTFX_KEX_CFG(NIX_INTF_RX));
	cfg = (cfg >> 32) & 0x07;
	seq_printf(filp, "\t\t RX keywidth \t: %s\n", (cfg == NPC_MCAM_KEY_X1) ?
		   "112bits" : ((cfg == NPC_MCAM_KEY_X2) ?
		   "224bits" : "448bits"));
	cfg = rvu_read64(rvu, blkaddr, NPC_AF_INTFX_KEX_CFG(NIX_INTF_TX));
	cfg = (cfg >> 32) & 0x07;
	seq_printf(filp, "\t\t TX keywidth \t: %s\n", (cfg == NPC_MCAM_KEY_X1) ?
		   "112bits" : ((cfg == NPC_MCAM_KEY_X2) ?
		   "224bits" : "448bits"));

	mutex_lock(&mcam->lock);
	/* MCAM entries */
	seq_printf(filp, "\n\t\t MCAM entries \t: %d\n", mcam->total_entries);
	seq_printf(filp, "\t\t Reserved \t: %d\n",
		   mcam->total_entries - mcam->bmap_entries);
	seq_printf(filp, "\t\t Available \t: %d\n", mcam->bmap_fcnt);

	/* MCAM counters */
	cfg = rvu_read64(rvu, blkaddr, NPC_AF_CONST);
	cfg = (cfg >> 48) & 0xFFFF;
	seq_printf(filp, "\n\t\t MCAM counters \t: %lld\n", cfg);
	seq_printf(filp, "\t\t Reserved \t: %lld\n", cfg - mcam->counters.max);
	seq_printf(filp, "\t\t Available \t: %d\n",
		   rvu_rsrc_free_count(&mcam->counters));

	if (mcam->bmap_entries == mcam->bmap_fcnt)
		return 0;

	seq_puts(filp, "\n\t\t Current allocation\n");
	seq_puts(filp, "\t\t====================\n");
	for (pf = 0; pf < rvu->hw->total_pfs; pf++) {
		pcifunc = (pf << RVU_PFVF_PF_SHIFT);
		rvu_print_npc_mcam_info(filp, pcifunc, blkaddr);

		cfg = rvu_read64(rvu, BLKADDR_RVUM, RVU_PRIV_PFX_CFG(pf));
		numvfs = (cfg >> 12) & 0xFF;
		for (vf = 0; vf < numvfs; vf++) {
			pcifunc = (pf << RVU_PFVF_PF_SHIFT) | (vf + 1);
			rvu_print_npc_mcam_info(filp, pcifunc, blkaddr);
		}
	}

	mutex_unlock(&mcam->lock);
	return 0;
}

RVU_DEBUG_SEQ_FOPS(npc_mcam_info, npc_mcam_info_display, NULL);

static int rvu_dbg_npc_rx_miss_stats_display(struct seq_file *filp,
					     void *unused)
{
	struct rvu *rvu = filp->private;
	struct npc_mcam *mcam;
	int blkaddr;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_NPC, 0);
	if (blkaddr < 0)
		return -ENODEV;

	mcam = &rvu->hw->mcam;

	seq_puts(filp, "\nNPC MCAM RX miss action stats\n");
	seq_printf(filp, "\t\tStat %d: \t%lld\n", mcam->rx_miss_act_cntr,
		   rvu_read64(rvu, blkaddr,
			      NPC_AF_MATCH_STATX(mcam->rx_miss_act_cntr)));

	return 0;
}

RVU_DEBUG_SEQ_FOPS(npc_rx_miss_act, npc_rx_miss_stats_display, NULL);

static void rvu_dbg_npc_mcam_show_flows(struct seq_file *s,
					struct rvu_npc_mcam_rule *rule)
{
	u8 bit;

	for_each_set_bit(bit, (unsigned long *)&rule->features, 64) {
		seq_printf(s, "\t%s  ", npc_get_field_name(bit));
		switch (bit) {
		case NPC_DMAC:
			seq_printf(s, "%pM ", rule->packet.dmac);
			seq_printf(s, "%pM\n", rule->mask.dmac);
			break;
		case NPC_SMAC:
			seq_printf(s, "%pM ", rule->packet.smac);
			seq_printf(s, "%pM\n", rule->mask.smac);
			break;
		case NPC_ETYPE:
			seq_printf(s, "0x%x ", ntohs(rule->packet.etype));
			seq_printf(s, "0x%x\n", ntohs(rule->mask.etype));
			break;
		case NPC_OUTER_VID:
			seq_printf(s, "%d ", ntohs(rule->packet.vlan_tci));
			seq_printf(s, "mask 0x%x\n",
				   ntohs(rule->mask.vlan_tci));
			break;
		case NPC_TOS:
			seq_printf(s, "%d ", rule->packet.tos);
			seq_printf(s, "mask 0x%x\n", rule->mask.tos);
			break;
		case NPC_SIP_IPV4:
			seq_printf(s, "%pI4 ", &rule->packet.ip4src);
			seq_printf(s, "%pI4\n", &rule->mask.ip4src);
			break;
		case NPC_DIP_IPV4:
			seq_printf(s, "%pI4 ", &rule->packet.ip4dst);
			seq_printf(s, "%pI4\n", &rule->mask.ip4dst);
			break;
		case NPC_SIP_IPV6:
			seq_printf(s, "%pI6 ", rule->packet.ip6src);
			seq_printf(s, "%pI6\n", rule->mask.ip6src);
			break;
		case NPC_DIP_IPV6:
			seq_printf(s, "%pI6 ", rule->packet.ip6dst);
			seq_printf(s, "%pI6\n", rule->mask.ip6dst);
			break;
		case NPC_SPORT_TCP:
		case NPC_SPORT_UDP:
			seq_printf(s, "%d ", ntohs(rule->packet.sport));
			seq_printf(s, "mask 0x%x\n", ntohs(rule->mask.sport));
			break;
		case NPC_DPORT_TCP:
		case NPC_DPORT_UDP:
			seq_printf(s, "%d ", ntohs(rule->packet.dport));
			seq_printf(s, "mask 0x%x\n", ntohs(rule->mask.dport));
			break;
		default:
			break;
		}
	}
}

static void rvu_dbg_npc_mcam_show_action(struct seq_file *s,
					 struct rvu_npc_mcam_rule *rule)
{
	switch (rule->action.op) {
	case NIX_RX_ACTIONOP_DROP:
		seq_puts(s, "\taction: Drop\n");
		break;
	case NIX_RX_ACTIONOP_UCAST:
		seq_printf(s, "\taction: Direct to queue %d\n",
			   rule->action.index);
		break;
	case NIX_RX_ACTIONOP_RSS:
		seq_puts(s, "\taction: RSS\n");
		break;
	case NIX_RX_ACTIONOP_UCAST_IPSEC:
		seq_puts(s, "\taction: Unicast ipsec\n");
		break;
	case NIX_RX_ACTIONOP_MCAST:
		seq_puts(s, "\taction: Multicast\n");
		break;
	default:
		break;
	};
}

static int rvu_dbg_npc_mcam_show_rules(struct seq_file *s, void *unused)
{
	struct rvu_npc_mcam_rule *iter;
	struct rvu *rvu = s->private;
	struct npc_mcam *mcam;
	int pf, vf = -1;
	int blkaddr;
	u16 target;
	u64 hits;

	mcam = &rvu->hw->mcam;

	list_for_each_entry(iter, &mcam->mcam_rules, list) {
		target = iter->action.pf_func;

		pf = (iter->owner >> RVU_PFVF_PF_SHIFT) & RVU_PFVF_PF_MASK;
		seq_printf(s, "\n\tPF%d ", pf);

		if (iter->owner & RVU_PFVF_FUNC_MASK) {
			vf = (iter->owner & RVU_PFVF_FUNC_MASK) - 1;
			seq_printf(s, "VF%d", vf);
		}
		seq_puts(s, "\n");

		pf = (target >> RVU_PFVF_PF_SHIFT) & RVU_PFVF_PF_MASK;
		seq_printf(s, "\ttarget: PF%d ", pf);

		if (target & RVU_PFVF_FUNC_MASK) {
			vf = (target & RVU_PFVF_FUNC_MASK) - 1;
			seq_printf(s, "VF%d", vf);
		}
		seq_puts(s, "\n");

		seq_printf(s, "\tmcam entry: %d\n", iter->entry);
		rvu_dbg_npc_mcam_show_flows(s, iter);
		rvu_dbg_npc_mcam_show_action(s, iter);
		seq_printf(s, "\tenabled: %s\n", iter->enable ? "yes" : "no");

		if (!iter->has_cntr)
			continue;
		seq_printf(s, "\tcounter: %d\n", iter->cntr);

		blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_NPC, 0);
		if (blkaddr < 0)
			return 0;
		hits = rvu_read64(rvu, blkaddr, NPC_AF_MATCH_STATX(iter->cntr));
		seq_printf(s, "\thits: %lld\n", hits);
	}

	return 0;
}

RVU_DEBUG_SEQ_FOPS(npc_mcam_rules, npc_mcam_show_rules, NULL);

static void rvu_dbg_npc_init(struct rvu *rvu)
{
	const struct device *dev = &rvu->pdev->dev;
	struct dentry *pfile;

	rvu->rvu_dbg.npc = debugfs_create_dir("npc", rvu->rvu_dbg.root);
	if (!rvu->rvu_dbg.npc)
		return;

	pfile = debugfs_create_file("mcam_info", 0444, rvu->rvu_dbg.npc,
				    rvu, &rvu_dbg_npc_mcam_info_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("mcam_rules", 0444, rvu->rvu_dbg.npc,
				    rvu, &rvu_dbg_npc_mcam_rules_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("rx_miss_act_stats", 0444, rvu->rvu_dbg.npc,
				    rvu, &rvu_dbg_npc_rx_miss_act_fops);
	if (!pfile)
		goto create_failed;

	return;

create_failed:
	dev_err(dev, "Failed to create debugfs dir/file for NPC\n");
	debugfs_remove_recursive(rvu->rvu_dbg.npc);
}

static int parse_sso_cmd_buffer(char *cmd_buf, size_t *count,
				const char __user *buffer, int *ssolf,
				bool *all)
{
	int ret, bytes_not_copied;
	char *cmd_buf_tmp;
	char *subtoken;

	bytes_not_copied = copy_from_user(cmd_buf, buffer, *count);
	if (bytes_not_copied)
		return -EFAULT;

	cmd_buf[*count] = '\0';
	cmd_buf_tmp = strchr(cmd_buf, '\n');

	if (cmd_buf_tmp) {
		*cmd_buf_tmp = '\0';
		*count = cmd_buf_tmp - cmd_buf + 1;
	}

	subtoken = strsep(&cmd_buf, " ");
	if (subtoken && strcmp(subtoken, "all") == 0) {
		*all = true;
	} else{
		ret = subtoken ? kstrtoint(subtoken, 10, ssolf) : -EINVAL;
		if (ret < 0)
			return ret;
	}
	if (cmd_buf)
		return -EINVAL;

	return 0;
}

static void sso_hwgrp_display_iq_list(struct rvu *rvu, int ssolf, u16 idx,
				      u16 tail_idx, u8 queue_type)
{
	const char *queue[3] = {"DQ", "CQ", "AQ"};
	int blkaddr;
	u64 reg;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_SSO, 0);
	if (blkaddr < 0)
		return;

	pr_info("SSO HWGGRP[%d] [%s] Chain queue head[%d]", ssolf,
		queue[queue_type], idx);
	pr_info("SSO HWGGRP[%d] [%s] Chain queue tail[%d]", ssolf,
		queue[queue_type], tail_idx);
	pr_info("--------------------------------------------------\n");
	do {
		reg = rvu_read64(rvu, blkaddr, SSO_AF_IENTX_TAG(idx));
		pr_info("SSO HWGGRP[%d] [%s] IE[%d] TAG      0x%llx\n", ssolf,
			queue[queue_type], idx, reg);

		reg = rvu_read64(rvu, blkaddr, SSO_AF_IENTX_GRP(idx));
		pr_info("SSO HWGGRP[%d] [%s] IE[%d] GRP      0x%llx\n", ssolf,
			queue[queue_type], idx, reg);

		reg = rvu_read64(rvu, blkaddr, SSO_AF_IENTX_PENDTAG(idx));
		pr_info("SSO HWGGRP[%d] [%s] IE[%d] PENDTAG  0x%llx\n", ssolf,
			queue[queue_type], idx, reg);

		reg = rvu_read64(rvu, blkaddr, SSO_AF_IENTX_LINKS(idx));
		pr_info("SSO HWGGRP[%d] [%s] IE[%d] LINKS    0x%llx\n", ssolf,
			queue[queue_type], idx, reg);

		reg = rvu_read64(rvu, blkaddr, SSO_AF_IENTX_QLINKS(idx));
		pr_info("SSO HWGGRP[%d] [%s] IE[%d] QLINKS   0x%llx\n", ssolf,
			queue[queue_type], idx, reg);
		pr_info("--------------------------------------------------\n");
		if (idx == tail_idx)
			break;
		idx = reg & 0x1FFF;
	} while (idx != 0x1FFF);
}

static void sso_hwgrp_display_taq_list(struct rvu *rvu, int ssolf, u8 wae_head,
				       u16 ent_head, u8 wae_used, u8 taq_lines)
{
	int i, blkaddr;
	u64 reg;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_SSO, 0);
	if (blkaddr < 0)
		return;

	pr_info("--------------------------------------------------\n");
	do {
		for (i = wae_head; i < taq_lines && wae_used; i++) {
			reg = rvu_read64(rvu, blkaddr,
					 SSO_AF_TAQX_WAEY_TAG(ent_head, i));
			pr_info("SSO HWGGRP[%d] TAQ[%d] WAE[%d] TAG  0x%llx\n",
				ssolf, ent_head, i, reg);

			reg = rvu_read64(rvu, blkaddr,
					 SSO_AF_TAQX_WAEY_WQP(ent_head, i));
			pr_info("SSO HWGGRP[%d] TAQ[%d] WAE[%d] WQP  0x%llx\n",
				ssolf, ent_head, i, reg);
			wae_used--;
		}

		reg = rvu_read64(rvu, blkaddr,
				 SSO_AF_TAQX_LINK(ent_head));
		pr_info("SSO HWGGRP[%d] TAQ[%d] LINK         0x%llx\n",
			ssolf, ent_head, reg);
		ent_head = reg & 0x7FF;
		pr_info("--------------------------------------------------\n");
	} while (ent_head && wae_used);
}

static int read_sso_pc(struct rvu *rvu)
{
	int blkaddr;
	u64 reg;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_SSO, 0);
	if (blkaddr < 0)
		return -ENODEV;

	reg = rvu_read64(rvu, blkaddr, SSO_AF_ACTIVE_CYCLES0);
	pr_info("SSO Add-Work active cycles		%lld\n", reg);
	reg = rvu_read64(rvu, blkaddr, SSO_AF_ACTIVE_CYCLES1);
	pr_info("SSO Get-Work active cycles		%lld\n", reg);
	reg = rvu_read64(rvu, blkaddr, SSO_AF_ACTIVE_CYCLES2);
	pr_info("SSO Work-Slot active cycles		%lld\n", reg);
	pr_info("\n");

	reg = rvu_read64(rvu, blkaddr, SSO_AF_NOS_CNT) & 0x1FFF;
	pr_info("SSO work-queue entries on the no-schedule list	%lld\n", reg);
	pr_info("\n");

	reg = rvu_read64(rvu, blkaddr, SSO_AF_AW_READ_ARB);
	pr_info("SSO XAQ reads outstanding		%lld\n",
		(reg & 0x3F) >> 24);

	reg = rvu_read64(rvu, blkaddr, SSO_AF_XAQ_REQ_PC);
	pr_info("SSO XAQ reads requests			%lld\n", reg);
	reg = rvu_read64(rvu, blkaddr, SSO_AF_XAQ_LATENCY_PC);
	pr_info("SSO XAQ read latency cycles		%lld\n", reg);
	pr_info("\n");

	reg = rvu_read64(rvu, blkaddr, SSO_AF_AW_WE);
	pr_info("SSO IAQ reserved			%lld\n",
		(reg & 0x3FFF) >> 16);
	pr_info("SSO IAQ total				%lld\n", reg & 0x3FFF);
	pr_info("\n");

	reg = rvu_read64(rvu, blkaddr, SSO_AF_TAQ_CNT);
	pr_info("SSO TAQ reserved			%lld\n",
		(reg & 0x7FF) >> 16);
	pr_info("SSO TAQ total				%lld\n", reg & 0x7FF);
	pr_info("\n");

	return 0;
}

/* Reads SSO hwgrp perfomance counters */
static void read_sso_hwgrp_pc(struct rvu *rvu, int ssolf, bool all)
{
	struct rvu_hwinfo *hw = rvu->hw;
	struct rvu_block *block;
	int blkaddr, max_id;
	u64 reg;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_SSO, 0);
	if (blkaddr < 0)
		return;

	block = &hw->block[blkaddr];
	if (ssolf < 0 || ssolf >= block->lf.max) {
		pr_info("Invalid SSOLF(HWGRP), valid range is 0-%d\n",
			block->lf.max - 1);
		return;
	}
	max_id =  block->lf.max;

	if (all)
		ssolf = 0;
	else
		max_id = ssolf + 1;

	pr_info("==================================================\n");
	for (; ssolf < max_id; ssolf++) {
		reg = rvu_read64(rvu, blkaddr, SSO_AF_HWGRPX_WS_PC(ssolf));
		pr_info("SSO HWGGRP[%d] Work-Schedule PC     0x%llx\n", ssolf,
			reg);

		reg = rvu_read64(rvu, blkaddr, SSO_AF_HWGRPX_EXT_PC(ssolf));
		pr_info("SSO HWGGRP[%d] External Schedule PC 0x%llx\n", ssolf,
			reg);

		reg = rvu_read64(rvu, blkaddr, SSO_AF_HWGRPX_WA_PC(ssolf));
		pr_info("SSO HWGGRP[%d] Work-Add PC          0x%llx\n", ssolf,
			reg);

		reg = rvu_read64(rvu, blkaddr, SSO_AF_HWGRPX_TS_PC(ssolf));
		pr_info("SSO HWGGRP[%d] Tag Switch PC        0x%llx\n", ssolf,
			reg);

		reg = rvu_read64(rvu, blkaddr, SSO_AF_HWGRPX_DS_PC(ssolf));
		pr_info("SSO HWGGRP[%d] Deschedule PC        0x%llx\n", ssolf,
			reg);

		reg = rvu_read64(rvu, blkaddr, SSO_AF_HWGRPX_DQ_PC(ssolf));
		pr_info("SSO HWGGRP[%d] Work-Descheduled PC  0x%llx\n", ssolf,
			reg);

		reg = rvu_read64(rvu, blkaddr,
				 SSO_AF_HWGRPX_PAGE_CNT(ssolf));
		pr_info("SSO HWGGRP[%d] In-use Page Count    0x%llx\n", ssolf,
			reg);
		pr_info("==================================================\n");
	}
}

/* Reads SSO hwgrp Threshold */
static void read_sso_hwgrp_thresh(struct rvu *rvu, int ssolf, bool all)
{
	struct rvu_hwinfo *hw = rvu->hw;
	struct rvu_block *block;
	int blkaddr, max_id;
	u64 reg;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_SSO, 0);
	if (blkaddr < 0)
		return;

	block = &hw->block[blkaddr];
	if (ssolf < 0 || ssolf >= block->lf.max) {
		pr_info("Invalid SSOLF(HWGRP), valid range is 0-%d\n",
			block->lf.max - 1);
		return;
	}
	max_id =  block->lf.max;

	if (all)
		ssolf = 0;
	else
		max_id = ssolf + 1;

	pr_info("==================================================\n");
	for (; ssolf < max_id; ssolf++) {
		reg = rvu_read64(rvu, blkaddr,
				 SSO_AF_HWGRPX_IAQ_THR(ssolf));
		pr_info("SSO HWGGRP[%d] IAQ Threshold        0x%llx\n", ssolf,
			reg);

		reg = rvu_read64(rvu, blkaddr,
				 SSO_AF_HWGRPX_TAQ_THR(ssolf));
		pr_info("SSO HWGGRP[%d] TAQ Threshold        0x%llx\n", ssolf,
			reg);

		reg = rvu_read64(rvu, blkaddr,
				 SSO_AF_HWGRPX_XAQ_AURA(ssolf));
		pr_info("SSO HWGGRP[%d] XAQ Aura             0x%llx\n", ssolf,
			reg);

		reg = rvu_read64(rvu, blkaddr,
				 SSO_AF_HWGRPX_XAQ_LIMIT(ssolf));
		pr_info("SSO HWGGRP[%d] XAQ Limit            0x%llx\n", ssolf,
			reg);

		reg = rvu_read64(rvu, blkaddr,
				 SSO_AF_HWGRPX_IU_ACCNT(ssolf));
		pr_info("SSO HWGGRP[%d] IU Account Index     0x%llx\n", ssolf,
			reg);

		reg = rvu_read64(rvu, blkaddr,
				 SSO_AF_IU_ACCNTX_CFG(reg & 0xFF));
		pr_info("SSO HWGGRP[%d] IU Accounting Cfg    0x%llx\n", ssolf,
			reg);
		pr_info("==================================================\n");
	}
}

/* Reads SSO hwgrp TAQ list */
static void read_sso_hwgrp_taq_list(struct rvu *rvu, int ssolf, bool all)
{
	struct rvu_hwinfo *hw = rvu->hw;
	u8 taq_entries, wae_head;
	struct rvu_block *block;
	u16 ent_head, cl_used;
	int blkaddr, max_id;
	u64 reg;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_SSO, 0);
	if (blkaddr < 0)
		return;

	block = &hw->block[blkaddr];
	if (ssolf < 0 || ssolf >= block->lf.max) {
		pr_info("Invalid SSOLF(HWGRP), valid range is 0-%d\n",
			block->lf.max - 1);
		return;
	}
	max_id =  block->lf.max;

	if (all)
		ssolf = 0;
	else
		max_id = ssolf + 1;
	reg = rvu_read64(rvu, blkaddr, SSO_AF_CONST);
	taq_entries = (reg >> 48) & 0xFF;
	pr_info("==================================================\n");
	for (; ssolf < max_id; ssolf++) {
		pr_info("++++++++++++++++++++++++++++++++++++++++++++++++++\n");
		pr_info("SSO HWGGRP[%d] Transitory Output Admission Queue",
			ssolf);
		reg = rvu_read64(rvu, blkaddr, SSO_AF_TOAQX_STATUS(ssolf));
		pr_info("SSO HWGGRP[%d] TOAQ Status          0x%llx\n", ssolf,
			reg);
		ent_head = (reg >> 12) & 0x7FF;
		cl_used = (reg >> 32) & 0x7FF;
		if (reg & BIT_ULL(61) && cl_used) {
			pr_info("SSO HWGGRP[%d] TOAQ CL_USED         0x%x\n",
				ssolf, cl_used);
			sso_hwgrp_display_taq_list(rvu, ssolf, ent_head, 0,
						   cl_used * taq_entries,
						   taq_entries);
		}
		pr_info("++++++++++++++++++++++++++++++++++++++++++++++++++\n");
		pr_info("SSO HWGGRP[%d] Transitory Input Admission Queue",
			ssolf);
		reg = rvu_read64(rvu, blkaddr, SSO_AF_TIAQX_STATUS(ssolf));
		pr_info("SSO HWGGRP[%d] TIAQ Status          0x%llx\n", ssolf,
			reg);
		wae_head = (reg >> 60) & 0xF;
		cl_used = (reg >> 32) & 0x7FFF;
		ent_head = (reg >> 12) & 0x7FF;
		if (reg & BIT_ULL(61) && cl_used) {
			pr_info("SSO HWGGRP[%d] TIAQ WAE_USED         0x%x\n",
				ssolf, cl_used);
			sso_hwgrp_display_taq_list(rvu, ssolf, ent_head,
						   wae_head, cl_used,
						   taq_entries);
		}
		pr_info("++++++++++++++++++++++++++++++++++++++++++++++++++\n");
		pr_info("==================================================\n");
	}
}

/* Reads SSO hwgrp IAQ list */
static void read_sso_hwgrp_iaq_list(struct rvu *rvu, int ssolf, bool all)
{
	struct rvu_hwinfo *hw = rvu->hw;
	struct rvu_block *block;
	u16 head_idx, tail_idx;
	int blkaddr, max_id;
	u64 reg;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_SSO, 0);
	if (blkaddr < 0)
		return;

	block = &hw->block[blkaddr];
	if (ssolf < 0 || ssolf >= block->lf.max) {
		pr_info("Invalid SSOLF(HWGRP), valid range is 0-%d\n",
			block->lf.max - 1);
		return;
	}
	max_id =  block->lf.max;

	if (all)
		ssolf = 0;
	else
		max_id = ssolf + 1;
	pr_info("==================================================\n");
	for (; ssolf < max_id; ssolf++) {
		pr_info("++++++++++++++++++++++++++++++++++++++++++++++++++\n");
		pr_info("SSO HWGGRP[%d] Deschedule Queue(DQ)\n", ssolf);
		reg = rvu_read64(rvu, blkaddr, SSO_AF_IPL_DESCHEDX(ssolf));
		pr_info("SSO HWGGRP[%d] DQ List              0x%llx\n", ssolf,
			reg);
		head_idx = (reg >> 13) & 0x1FFF;
		tail_idx = reg & 0x1FFF;
		if (reg & (BIT_ULL(26) | BIT_ULL(27)))
			sso_hwgrp_display_iq_list(rvu, ssolf, head_idx,
						  tail_idx, 0);
		pr_info("++++++++++++++++++++++++++++++++++++++++++++++++++\n");
		pr_info("SSO HWGGRP[%d] Conflict Queue(CQ)\n", ssolf);
		reg = rvu_read64(rvu, blkaddr, SSO_AF_IPL_CONFX(ssolf));
		pr_info("SSO HWGGRP[%d] CQ List              0x%llx\n", ssolf,
			reg);
		head_idx = (reg >> 13) & 0x1FFF;
		tail_idx = reg & 0x1FFF;
		if (reg & (BIT_ULL(26) | BIT_ULL(27)))
			sso_hwgrp_display_iq_list(rvu, ssolf, head_idx,
						  tail_idx, 1);
		pr_info("++++++++++++++++++++++++++++++++++++++++++++++++++\n");
		pr_info("SSO HWGGRP[%d] Admission Queue(AQ)\n", ssolf);
		reg = rvu_read64(rvu, blkaddr, SSO_AF_IPL_IAQX(ssolf));
		pr_info("SSO HWGGRP[%d] AQ List              0x%llx\n", ssolf,
			reg);
		head_idx = (reg >> 13) & 0x1FFF;
		tail_idx = reg & 0x1FFF;
		if (reg & (BIT_ULL(26) | BIT_ULL(27)))
			sso_hwgrp_display_iq_list(rvu, ssolf, head_idx,
						  tail_idx, 2);
		pr_info("++++++++++++++++++++++++++++++++++++++++++++++++++\n");
		pr_info("==================================================\n");
	}
}

/* Reads SSO hwgrp IENT list */
static int read_sso_hwgrp_ient_list(struct rvu *rvu)
{
	const char *tt_c[4] = {"SSO_TT_ORDERED_", "SSO_TT_ATOMIC__",
				"SSO_TT_UNTAGGED", "SSO_TT_EMPTY___"};
	struct rvu_hwinfo *hw = rvu->hw;
	int max_idx = hw->sso.sso_iue;
	u64 pendtag, qlinks, links;
	int len, idx, blkaddr;
	u64 tag, grp, wqp;
	char str[300];

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_SSO, 0);
	if (blkaddr < 0)
		return -ENODEV;

	for (idx = 0; idx < max_idx; idx++) {
		len = 0;
		tag = rvu_read64(rvu, blkaddr, SSO_AF_IENTX_TAG(idx));
		grp = rvu_read64(rvu, blkaddr, SSO_AF_IENTX_GRP(idx));
		pendtag = rvu_read64(rvu, blkaddr,
				     SSO_AF_IENTX_PENDTAG(idx));
		links = rvu_read64(rvu, blkaddr, SSO_AF_IENTX_LINKS(idx));
		qlinks = rvu_read64(rvu, blkaddr,
				    SSO_AF_IENTX_QLINKS(idx));
		wqp = rvu_read64(rvu, blkaddr, SSO_AF_IENTX_WQP(idx));
		len = snprintf(str + len, 300,
			       "SSO IENT[%4d] TT [%s] HWGRP [%3lld] ", idx,
				tt_c[(tag >> 32) & 0x3], (grp >> 48) & 0x1f);
		len += snprintf(str + len, 300 - len,
				"TAG [0x%010llx] GRP [0x%016llx] ", tag, grp);
		len += snprintf(str + len, 300 - len, "PENDTAG [0x%010llx] ",
				pendtag);
		len += snprintf(str + len, 300 - len,
				"LINKS [0x%016llx] QLINKS [0x%010llx] ", links,
				qlinks);
		snprintf(str + len, 300 - len, "WQP [0x%016llx]\n", wqp);
		pr_info("%s", str);
	}

	return 0;
}

/* Reads SSO hwgrp free list */
static int read_sso_hwgrp_free_list(struct rvu *rvu)
{
	int blkaddr;
	u64 reg;
	u8 idx;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_SSO, 0);
	if (blkaddr < 0)
		return -ENODEV;

	pr_info("==================================================\n");
	for (idx = 0; idx < 4; idx++) {
		reg = rvu_read64(rvu, blkaddr, SSO_AF_IPL_FREEX(idx));
		pr_info("SSO FREE LIST[%d]\n", idx);
		pr_info("qnum_head : %lld qnum_tail : %lld\n",
			(reg >> 58) & 0x3, (reg >> 56) & 0x3);
		pr_info("queue_cnt : %llx\n", (reg >> 26) & 0x7fff);
		pr_info("queue_val : %lld queue_head : %4lld queue_tail %4lld\n"
			, (reg >> 40) & 0x1, (reg >> 13) & 0x1fff,
			reg & 0x1fff);
		pr_info("==================================================\n");
	}

	return 0;
}

/* Reads SSO hwgrp perfomance counters */
static void read_sso_hws_info(struct rvu *rvu, int ssowlf, bool all)
{
	struct rvu_hwinfo *hw = rvu->hw;
	struct rvu_block *block;
	int blkaddr;
	int max_id;
	u64 reg;
	u8 mask;
	u8 set;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_SSOW, 0);
	if (blkaddr < 0)
		return;

	block = &hw->block[blkaddr];
	if (ssowlf < 0 || ssowlf >= block->lf.max) {
		pr_info("Invalid SSOWLF(HWS), valid range is 0-%d\n",
			block->lf.max - 1);
		return;
	}
	max_id =  block->lf.max;

	if (all)
		ssowlf = 0;
	else
		max_id = ssowlf + 1;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_SSO, 0);
	if (blkaddr < 0)
		return;

	pr_info("==================================================\n");
	for (; ssowlf < max_id; ssowlf++) {
		reg = rvu_read64(rvu, blkaddr, SSO_AF_HWSX_ARB(ssowlf));
		pr_info("SSOW HWS[%d] Arbitration State      0x%llx\n", ssowlf,
			reg);
		reg = rvu_read64(rvu, blkaddr, SSO_AF_HWSX_GMCTL(ssowlf));
		pr_info("SSOW HWS[%d] Guest Machine Control  0x%llx\n", ssowlf,
			reg);
		for (set = 0; set < 2; set++)
			for (mask = 0; mask < 4; mask++) {
				reg = rvu_read64(rvu, blkaddr,
						 SSO_AF_HWSX_SX_GRPMSKX(ssowlf,
									set,
									mask));
				pr_info(
				"SSOW HWS[%d] SET[%d] Group Mask[%d] 0x%llx\n",
				ssowlf, set, mask, reg);
			}
		pr_info("==================================================\n");
	}
}

typedef void (*sso_dump_cb)(struct rvu *rvu, int ssolf, bool all);

static ssize_t rvu_dbg_sso_cmd_parser(struct file *filp,
				      const char __user *buffer, size_t count,
				      loff_t *ppos, char *lf_type,
				      char *file_nm, sso_dump_cb fn)
{
	struct rvu *rvu = filp->private_data;
	bool all = false;
	char *cmd_buf;
	int lf = 0;

	if (*ppos != 0)
		return 0;

	cmd_buf = kzalloc(count + 1, GFP_KERNEL);

	if (!cmd_buf || !count)
		return count;
	if (parse_sso_cmd_buffer(cmd_buf, &count, buffer,
				 &lf, &all) < 0) {
		pr_info("Usage: echo [<%s>/all] > %s\n", lf_type, file_nm);
	} else {
		fn(rvu, lf, all);
	}
	kfree(cmd_buf);

	return count;
}

/* SSO debugfs APIs */
static ssize_t rvu_dbg_sso_pc_display(struct file *filp,
				      char __user *buffer,
				      size_t count, loff_t *ppos)
{
	return read_sso_pc(filp->private_data);
}

static ssize_t rvu_dbg_sso_hwgrp_pc_display(struct file *filp,
					    const char __user *buffer,
					    size_t count, loff_t *ppos)
{
	return rvu_dbg_sso_cmd_parser(filp, buffer, count, ppos, "hwgrp",
			"sso_hwgrp_pc", read_sso_hwgrp_pc);
}

static ssize_t rvu_dbg_sso_hwgrp_thresh_display(struct file *filp,
						const char __user *buffer,
						size_t count, loff_t *ppos)
{
	return rvu_dbg_sso_cmd_parser(filp, buffer, count, ppos, "hwgrp",
			"sso_hwgrp_thresh", read_sso_hwgrp_thresh);
}

static ssize_t rvu_dbg_sso_hwgrp_taq_wlk_display(struct file *filp,
						 const char __user *buffer,
						 size_t count, loff_t *ppos)
{
	return rvu_dbg_sso_cmd_parser(filp, buffer, count, ppos, "hwgrp",
			"sso_hwgrp_taq_wlk", read_sso_hwgrp_taq_list);
}

static ssize_t rvu_dbg_sso_hwgrp_iaq_wlk_display(struct file *filp,
						 const char __user *buffer,
						 size_t count, loff_t *ppos)
{
	return rvu_dbg_sso_cmd_parser(filp, buffer, count, ppos, "hwgrp",
			"sso_hwgrp_iaq_wlk", read_sso_hwgrp_iaq_list);
}

static ssize_t rvu_dbg_sso_hwgrp_ient_wlk_display(struct file *filp,
						  char __user *buffer,
						  size_t count, loff_t *ppos)
{
	return read_sso_hwgrp_ient_list(filp->private_data);
}

static ssize_t rvu_dbg_sso_hwgrp_fl_wlk_display(struct file *filp,
						char __user *buffer,
						size_t count, loff_t *ppos)
{
	return read_sso_hwgrp_free_list(filp->private_data);
}

static ssize_t rvu_dbg_sso_hws_info_display(struct file *filp,
					    const char __user *buffer,
					    size_t count, loff_t *ppos)
{
	return rvu_dbg_sso_cmd_parser(filp, buffer, count, ppos, "hws",
			"sso_hws_info", read_sso_hws_info);
}

RVU_DEBUG_FOPS(sso_pc, sso_pc_display, NULL);
RVU_DEBUG_FOPS(sso_hwgrp_pc, NULL, sso_hwgrp_pc_display);
RVU_DEBUG_FOPS(sso_hwgrp_thresh, NULL, sso_hwgrp_thresh_display);
RVU_DEBUG_FOPS(sso_hwgrp_taq_wlk, NULL, sso_hwgrp_taq_wlk_display);
RVU_DEBUG_FOPS(sso_hwgrp_iaq_wlk, NULL, sso_hwgrp_iaq_wlk_display);
RVU_DEBUG_FOPS(sso_hwgrp_ient_wlk, sso_hwgrp_ient_wlk_display, NULL);
RVU_DEBUG_FOPS(sso_hwgrp_fl_wlk, sso_hwgrp_fl_wlk_display, NULL);
RVU_DEBUG_FOPS(sso_hws_info, NULL, sso_hws_info_display);

static void rvu_dbg_sso_init(struct rvu *rvu)
{
	const struct device *dev = &rvu->pdev->dev;
	struct dentry *pfile;

	rvu->rvu_dbg.sso = debugfs_create_dir("sso", rvu->rvu_dbg.root);
	if (!rvu->rvu_dbg.sso)
		return;

	rvu->rvu_dbg.sso_hwgrp = debugfs_create_dir("hwgrp", rvu->rvu_dbg.sso);
	if (!rvu->rvu_dbg.sso_hwgrp)
		return;

	rvu->rvu_dbg.sso_hws = debugfs_create_dir("hws", rvu->rvu_dbg.sso);
	if (!rvu->rvu_dbg.sso_hws)
		return;

	pfile = debugfs_create_file("sso_pc", 0600,
				    rvu->rvu_dbg.sso, rvu,
			&rvu_dbg_sso_pc_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("sso_hwgrp_pc", 0600,
				    rvu->rvu_dbg.sso_hwgrp, rvu,
			&rvu_dbg_sso_hwgrp_pc_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("sso_hwgrp_thresh", 0600,
				    rvu->rvu_dbg.sso_hwgrp, rvu,
			&rvu_dbg_sso_hwgrp_thresh_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("sso_hwgrp_taq_walk", 0600,
				    rvu->rvu_dbg.sso_hwgrp, rvu,
			&rvu_dbg_sso_hwgrp_taq_wlk_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("sso_hwgrp_iaq_walk", 0600,
				    rvu->rvu_dbg.sso_hwgrp, rvu,
			&rvu_dbg_sso_hwgrp_iaq_wlk_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("sso_hwgrp_ient_walk", 0600,
				    rvu->rvu_dbg.sso_hwgrp, rvu,
			&rvu_dbg_sso_hwgrp_ient_wlk_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("sso_hwgrp_free_list_walk", 0600,
				    rvu->rvu_dbg.sso_hwgrp, rvu,
			&rvu_dbg_sso_hwgrp_fl_wlk_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("sso_hws_info", 0600,
				    rvu->rvu_dbg.sso_hws, rvu,
			&rvu_dbg_sso_hws_info_fops);
	if (!pfile)
		goto create_failed;

	return;

create_failed:
	dev_err(dev, "Failed to create debugfs dir/file for SSO\n");
	debugfs_remove_recursive(rvu->rvu_dbg.sso);
}

/* CPT debugfs APIs */
static int parse_cpt_cmd_buffer(char *cmd_buf, size_t *count,
				const char __user *buffer, char *e_type)
{
	int  bytes_not_copied;
	char *cmd_buf_tmp;
	char *subtoken;

	bytes_not_copied = copy_from_user(cmd_buf, buffer, *count);
	if (bytes_not_copied)
		return -EFAULT;

	cmd_buf[*count] = '\0';
	cmd_buf_tmp = strchr(cmd_buf, '\n');

	if (cmd_buf_tmp) {
		*cmd_buf_tmp = '\0';
		*count = cmd_buf_tmp - cmd_buf + 1;
	}

	subtoken = strsep(&cmd_buf, " ");
	if (subtoken)
		strcpy(e_type, subtoken);
	else
		return -EINVAL;

	if (cmd_buf)
		return -EINVAL;

	if (strcmp(e_type, "SE") && strcmp(e_type, "IE") &&
	    strcmp(e_type, "AE") && strcmp(e_type, "all"))
		return -EINVAL;

	return 0;
}

static ssize_t rvu_dbg_cpt_cmd_parser(struct file *filp,
				      const char __user *buffer, size_t count,
				      loff_t *ppos)
{
	struct seq_file *s = filp->private_data;
	struct rvu *rvu = s->private;
	char *cmd_buf;
	int ret = 0;

	if (*ppos != 0)
		return -EINVAL;

	cmd_buf = kzalloc(count + 1, GFP_KERNEL);
	if (!cmd_buf)
		return -ENOSPC;
	if (!count) {
		kfree(cmd_buf);
		return count;
	}
	if (parse_cpt_cmd_buffer(cmd_buf, &count, buffer,
				 rvu->rvu_dbg.cpt_ctx.e_type) < 0)
		ret = -EINVAL;

	kfree(cmd_buf);

	if (ret)
		return -EINVAL;

	return count;
}

static ssize_t rvu_dbg_cpt_engines_sts_write(struct file *filp,
					     const char __user *buffer,
					     size_t count, loff_t *ppos)
{
	return rvu_dbg_cpt_cmd_parser(filp, buffer, count, ppos);
}

static int rvu_dbg_cpt_engines_sts_display(struct seq_file *filp, void *unused)
{
	u64  busy_sts[2] = {0}, free_sts[2] = {0};
	struct rvu *rvu = filp->private;
	u16  max_ses, max_ies, max_aes;
	u32  e_min = 0, e_max = 0, e;
	int  blkaddr;
	char *e_type;
	u64  reg;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_CPT, 0);
	if (blkaddr < 0)
		return -ENODEV;

	reg = rvu_read64(rvu, blkaddr, CPT_AF_CONSTANTS1);
	max_ses = reg & 0xffff;
	max_ies = (reg >> 16) & 0xffff;
	max_aes = (reg >> 32) & 0xffff;

	e_type = rvu->rvu_dbg.cpt_ctx.e_type;

	if (strcmp(e_type, "SE") == 0) {
		e_min = 0;
		e_max = max_ses - 1;
	} else if (strcmp(e_type, "IE") == 0) {
		e_min = max_ses;
		e_max = max_ses + max_ies - 1;
	} else if (strcmp(e_type, "AE") == 0) {
		e_min = max_ses + max_ies;
		e_max = max_ses + max_ies + max_aes - 1;
	} else if (strcmp(e_type, "all") == 0) {
		e_min = 0;
		e_max = max_ses + max_ies + max_aes - 1;
	} else {
		return -EINVAL;
	}

	for (e = e_min; e <= e_max; e++) {
		reg = rvu_read64(rvu, blkaddr, CPT_AF_EXEX_STS(e));
		if (reg & 0x1) {
			if (e < max_ses)
				busy_sts[0] |= 1ULL << e;
			else if (e >= max_ses)
				busy_sts[1] |= 1ULL << (e - max_ses);
		}
		if (reg & 0x2) {
			if (e < max_ses)
				free_sts[0] |= 1ULL << e;
			else if (e >= max_ses)
				free_sts[1] |= 1ULL << (e - max_ses);
		}
	}
	seq_printf(filp, "FREE STS : 0x%016llx  0x%016llx\n", free_sts[1],
		   free_sts[0]);
	seq_printf(filp, "BUSY STS : 0x%016llx  0x%016llx\n", busy_sts[1],
		   busy_sts[0]);

	return 0;
}

RVU_DEBUG_SEQ_FOPS(cpt_engines_sts, cpt_engines_sts_display,
		   cpt_engines_sts_write);

static ssize_t rvu_dbg_cpt_engines_info_write(struct file *filp,
					      const char __user *buffer,
					      size_t count, loff_t *ppos)
{
	return rvu_dbg_cpt_cmd_parser(filp, buffer, count, ppos);
}

static int rvu_dbg_cpt_engines_info_display(struct seq_file *filp, void *unused)
{
	struct rvu *rvu = filp->private;
	u16  max_ses, max_ies, max_aes;
	u32  e_min, e_max, e;
	int  blkaddr;
	char *e_type;
	u64  reg;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_CPT, 0);
	if (blkaddr < 0)
		return -ENODEV;

	reg = rvu_read64(rvu, blkaddr, CPT_AF_CONSTANTS1);
	max_ses = reg & 0xffff;
	max_ies = (reg >> 16) & 0xffff;
	max_aes = (reg >> 32) & 0xffff;

	e_type = rvu->rvu_dbg.cpt_ctx.e_type;

	if (strcmp(e_type, "SE") == 0) {
		e_min = 0;
		e_max = max_ses - 1;
	} else if (strcmp(e_type, "IE") == 0) {
		e_min = max_ses;
		e_max = max_ses + max_ies - 1;
	} else if (strcmp(e_type, "AE") == 0) {
		e_min = max_ses + max_ies;
		e_max = max_ses + max_ies + max_aes - 1;
	} else if (strcmp(e_type, "all") == 0) {
		e_min = 0;
		e_max = max_ses + max_ies + max_aes - 1;
	} else {
		return -EINVAL;
	}

	seq_puts(filp, "===========================================\n");
	for (e = e_min; e <= e_max; e++) {
		reg = rvu_read64(rvu, blkaddr, CPT_AF_EXEX_CTL2(e));
		seq_printf(filp, "CPT Engine[%u] Group Enable   0x%02llx\n", e,
			   reg & 0xff);
		reg = rvu_read64(rvu, blkaddr, CPT_AF_EXEX_ACTIVE(e));
		seq_printf(filp, "CPT Engine[%u] Active Info    0x%llx\n", e,
			   reg);
		reg = rvu_read64(rvu, blkaddr, CPT_AF_EXEX_CTL(e));
		seq_printf(filp, "CPT Engine[%u] Control        0x%llx\n", e,
			   reg);
		seq_puts(filp, "===========================================\n");
	}
	return 0;
}

RVU_DEBUG_SEQ_FOPS(cpt_engines_info, cpt_engines_info_display,
		   cpt_engines_info_write);

static int rvu_dbg_cpt_lfs_info_display(struct seq_file *filp, void *unused)
{
	struct rvu *rvu = filp->private;
	struct rvu_hwinfo *hw = rvu->hw;
	struct rvu_block *block;
	int blkaddr;
	u64 reg;
	u32 lf;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_CPT, 0);
	if (blkaddr < 0)
		return -ENODEV;

	block = &hw->block[blkaddr];
	if (!block->lf.bmap)
		return -ENODEV;

	seq_puts(filp, "===========================================\n");
	for (lf = 0; lf < block->lf.max; lf++) {
		reg = rvu_read64(rvu, blkaddr, CPT_AF_LFX_CTL(lf));
		seq_printf(filp, "CPT Lf[%u] CTL          0x%llx\n", lf, reg);
		reg = rvu_read64(rvu, blkaddr, CPT_AF_LFX_CTL2(lf));
		seq_printf(filp, "CPT Lf[%u] CTL2         0x%llx\n", lf, reg);
		reg = rvu_read64(rvu, blkaddr, CPT_AF_LFX_PTR_CTL(lf));
		seq_printf(filp, "CPT Lf[%u] PTR_CTL      0x%llx\n", lf, reg);
		reg = rvu_read64(rvu, blkaddr, block->lfcfg_reg |
				(lf << block->lfshift));
		seq_printf(filp, "CPT Lf[%u] CFG          0x%llx\n", lf, reg);
		seq_puts(filp, "===========================================\n");
	}
	return 0;
}

RVU_DEBUG_SEQ_FOPS(cpt_lfs_info, cpt_lfs_info_display, NULL);

static int rvu_dbg_cpt_err_info_display(struct seq_file *filp, void *unused)
{
	struct rvu *rvu = filp->private;
	u64 reg0, reg1;
	int blkaddr;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_CPT, 0);
	if (blkaddr < 0)
		return -ENODEV;

	reg0 = rvu_read64(rvu, blkaddr, CPT_AF_FLTX_INT(0));
	reg1 = rvu_read64(rvu, blkaddr, CPT_AF_FLTX_INT(1));
	seq_printf(filp, "CPT_AF_FLTX_INT:       0x%llx 0x%llx\n", reg0, reg1);
	reg0 = rvu_read64(rvu, blkaddr, CPT_AF_PSNX_EXE(0));
	reg1 = rvu_read64(rvu, blkaddr, CPT_AF_PSNX_EXE(1));
	seq_printf(filp, "CPT_AF_PSNX_EXE:       0x%llx 0x%llx\n", reg0, reg1);
	reg0 = rvu_read64(rvu, blkaddr, CPT_AF_PSNX_LF(0));
	seq_printf(filp, "CPT_AF_PSNX_LF:        0x%llx\n", reg0);
	reg0 = rvu_read64(rvu, blkaddr, CPT_AF_RVU_INT);
	seq_printf(filp, "CPT_AF_RVU_INT:        0x%llx\n", reg0);
	reg0 = rvu_read64(rvu, blkaddr, CPT_AF_RAS_INT);
	seq_printf(filp, "CPT_AF_RAS_INT:        0x%llx\n", reg0);
	reg0 = rvu_read64(rvu, blkaddr, CPT_AF_EXE_ERR_INFO);
	seq_printf(filp, "CPT_AF_EXE_ERR_INFO:   0x%llx\n", reg0);

	return 0;
}

RVU_DEBUG_SEQ_FOPS(cpt_err_info, cpt_err_info_display, NULL);

static int rvu_dbg_cpt_pc_display(struct seq_file *filp, void *unused)
{
	struct rvu *rvu;
	int blkaddr;
	u64 reg;

	rvu = filp->private;
	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_CPT, 0);
	if (blkaddr < 0)
		return -ENODEV;

	reg = rvu_read64(rvu, blkaddr, CPT_AF_INST_REQ_PC);
	seq_printf(filp, "CPT instruction requests   %llu\n", reg);
	reg = rvu_read64(rvu, blkaddr, CPT_AF_INST_LATENCY_PC);
	seq_printf(filp, "CPT instruction latency    %llu\n", reg);
	reg = rvu_read64(rvu, blkaddr, CPT_AF_RD_REQ_PC);
	seq_printf(filp, "CPT NCB read requests      %llu\n", reg);
	reg = rvu_read64(rvu, blkaddr, CPT_AF_RD_LATENCY_PC);
	seq_printf(filp, "CPT NCB read latency       %llu\n", reg);
	reg = rvu_read64(rvu, blkaddr, CPT_AF_RD_UC_PC);
	seq_printf(filp, "CPT read requests caused by UC fills   %llu\n", reg);
	reg = rvu_read64(rvu, blkaddr, CPT_AF_ACTIVE_CYCLES_PC);
	seq_printf(filp, "CPT active cycles pc       %llu\n", reg);
	reg = rvu_read64(rvu, blkaddr, CPT_AF_CPTCLK_CNT);
	seq_printf(filp, "CPT clock count pc         %llu\n", reg);

	return 0;
}

RVU_DEBUG_SEQ_FOPS(cpt_pc, cpt_pc_display, NULL);

static void rvu_dbg_cpt_init(struct rvu *rvu)
{
	const struct device *dev = &rvu->pdev->dev;
	struct dentry *pfile;

	rvu->rvu_dbg.cpt = debugfs_create_dir("cpt", rvu->rvu_dbg.root);
	if (!rvu->rvu_dbg.cpt)
		return;

	pfile = debugfs_create_file("cpt_pc", 0600,
				    rvu->rvu_dbg.cpt, rvu,
				    &rvu_dbg_cpt_pc_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("cpt_engines_sts", 0600,
				    rvu->rvu_dbg.cpt, rvu,
				    &rvu_dbg_cpt_engines_sts_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("cpt_engines_info", 0600,
				    rvu->rvu_dbg.cpt, rvu,
				    &rvu_dbg_cpt_engines_info_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("cpt_lfs_info", 0600,
				    rvu->rvu_dbg.cpt, rvu,
				    &rvu_dbg_cpt_lfs_info_fops);
	if (!pfile)
		goto create_failed;

	pfile = debugfs_create_file("cpt_err_info", 0600,
				    rvu->rvu_dbg.cpt, rvu,
				    &rvu_dbg_cpt_err_info_fops);
	if (!pfile)
		goto create_failed;

	return;

create_failed:
	dev_err(dev, "Failed to create debugfs dir/file for CPT\n");
	debugfs_remove_recursive(rvu->rvu_dbg.cpt);
}

void rvu_dbg_init(struct rvu *rvu)
{
	struct device *dev = &rvu->pdev->dev;
	struct dentry *pfile;

	rvu->rvu_dbg.root = debugfs_create_dir("octeontx2", NULL);
	if (!rvu->rvu_dbg.root) {
		dev_err(rvu->dev, "%s failed\n", __func__);
		return;
	}
	pfile = debugfs_create_file("rsrc_alloc", 0444, rvu->rvu_dbg.root, rvu,
				    &rvu_dbg_rsrc_status_fops);
	if (!pfile)
		goto create_failed;

	rvu_dbg_npa_init(rvu);

	rvu_dbg_cgx_init(rvu);

	rvu_dbg_nix_init(rvu);

	rvu_dbg_npc_init(rvu);

	rvu_dbg_sso_init(rvu);

	if (is_block_implemented(rvu->hw, BLKADDR_CPT0))
		rvu_dbg_cpt_init(rvu);

	return;

create_failed:
	dev_err(dev, "Failed to create debugfs dir\n");
	debugfs_remove_recursive(rvu->rvu_dbg.root);
}

void rvu_dbg_exit(struct rvu *rvu)
{
	debugfs_remove_recursive(rvu->rvu_dbg.root);
}
#endif /* CONFIG_DEBUG_FS */

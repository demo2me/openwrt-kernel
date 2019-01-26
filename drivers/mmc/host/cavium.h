/*
 * Driver for MMC and SSD cards for Cavium OCTEON and ThunderX SOCs.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2012-2017 Cavium Inc.
 */

#ifndef _CAVIUM_MMC_H_
#define _CAVIUM_MMC_H_

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/mmc/host.h>
#include <linux/of.h>
#include <linux/scatterlist.h>
#include <linux/semaphore.h>
#include <linux/pci.h>

#define CAVIUM_MAX_MMC		4
#define BLKSZ_EXT_CSD		512

/* Subsystem Device ID */
#define PCI_SUBSYS_DEVID_8XXX	0xA
#define PCI_SUBSYS_DEVID_9XXX	0xB

#define KHZ_400 (400000)
#define MHZ_26  (26000000)
#define MHZ_52  (52000000)
#define MHZ_100 (100000000)
#define MHZ_200 (200000000)

/* octtx2: emmc interface io current drive strength */
#define	MILLI_AMP_2	(0x0)
#define	MILLI_AMP_4	(0x1)
#define	MILLI_AMP_8	(0x2)
#define	MILLI_AMP_16	(0x3)

/* octtx2: emmc interface io clk skew */
#define	LOW_SLEW_RATE	(0x0)
#define	HIGH_SLEW_RATE	(0x1)

/* octtx2: emmc interface calibration */
#define START_CALIBRATION	(0x1)
#define TOTAL_NO_OF_TAPS	(512)
#define PS_10000		(10 * 1000)
#define PS_5000			(5000)
#define PS_2500			(2500)
#define PS_400			(400)
#define MAX_NO_OF_TAPS		64


/* DMA register addresses */
#define MIO_EMM_DMA_FIFO_CFG(x)	(0x00 + x->reg_off_dma)
#define MIO_EMM_DMA_FIFO_ADR(x)	(0x10 + x->reg_off_dma)
#define MIO_EMM_DMA_FIFO_CMD(x)	(0x18 + x->reg_off_dma)
#define MIO_EMM_DMA_CFG(x)	(0x20 + x->reg_off_dma)
#define MIO_EMM_DMA_ADR(x)	(0x28 + x->reg_off_dma)
#define MIO_EMM_DMA_INT(x)	(0x30 + x->reg_off_dma)
#define MIO_EMM_DMA_INT_W1S(x)	(0x38 + x->reg_off_dma)
#define MIO_EMM_DMA_INT_ENA_W1S(x) (0x40 + x->reg_off_dma)
#define MIO_EMM_DMA_INT_ENA_W1C(x) (0x48 + x->reg_off_dma)

/* octtx2 specific registers */
#define MIO_EMM_CALB(x)		(0xC0 + x->reg_off)
#define MIO_EMM_TAP(x)		(0xC8 + x->reg_off)
#define MIO_EMM_TIMING(x)	(0xD0 + x->reg_off)
#define MIO_EMM_DEBUG(x)	(0xF8 + x->reg_off)

/* register addresses */
#define MIO_EMM_CFG(x)		(0x00 + x->reg_off)
#define MIO_EMM_MODE(x, s)	(0x08 + 8*(s) + (x)->reg_off)
/* octtx2 specific register */
#define MIO_EMM_IO_CTL(x)	(0x40 + x->reg_off)
#define MIO_EMM_SWITCH(x)	(0x48 + x->reg_off)
#define MIO_EMM_DMA(x)		(0x50 + x->reg_off)
#define MIO_EMM_CMD(x)		(0x58 + x->reg_off)
#define MIO_EMM_RSP_STS(x)	(0x60 + x->reg_off)
#define MIO_EMM_RSP_LO(x)	(0x68 + x->reg_off)
#define MIO_EMM_RSP_HI(x)	(0x70 + x->reg_off)
#define MIO_EMM_INT(x)		(0x78 + x->reg_off)
#define MIO_EMM_INT_EN(x)	(0x80 + x->reg_off)
#define MIO_EMM_WDOG(x)		(0x88 + x->reg_off)
#define MIO_EMM_SAMPLE(x)	(0x90 + x->reg_off)
#define MIO_EMM_STS_MASK(x)	(0x98 + x->reg_off)
#define MIO_EMM_RCA(x)		(0xa0 + x->reg_off)
#define MIO_EMM_INT_EN_SET(x)	(0xb0 + x->reg_off)
#define MIO_EMM_INT_EN_CLR(x)	(0xb8 + x->reg_off)
#define MIO_EMM_BUF_IDX(x)	(0xe0 + x->reg_off)
#define MIO_EMM_BUF_DAT(x)	(0xe8 + x->reg_off)

struct cvm_mmc_host {
	struct device *dev;
	void __iomem *base;
	void __iomem *dma_base;
	struct pci_dev *pdev;
	int reg_off;
	int reg_off_dma;
	u64 emm_cfg;
	u64 n_minus_one;	/* OCTEON II workaround location */
	int last_slot;
	struct clk *clk;
	int sys_freq;

	struct mmc_request *current_req;
	struct sg_mapping_iter smi;
	bool dma_active;
	bool use_sg;

	bool has_ciu3;
	bool big_dma_addr;
	bool need_irq_handler_lock;
	spinlock_t irq_handler_lock;
	struct semaphore mmc_serializer;

	struct gpio_desc *global_pwr_gpiod;
	atomic_t shared_power_users;
	bool powered;

	struct cvm_mmc_slot *slot[CAVIUM_MAX_MMC];
	struct platform_device *slot_pdev[CAVIUM_MAX_MMC];
	/* octtx2 specific */
	unsigned int per_tap_delay; /* per tap delay in pico second */
	struct delayed_work periodic_work;

	void (*set_shared_power)(struct cvm_mmc_host *, int);
	void (*acquire_bus)(struct cvm_mmc_host *);
	void (*release_bus)(struct cvm_mmc_host *);
	void (*int_enable)(struct cvm_mmc_host *, u64);
	/* required on some MIPS models */
	void (*dmar_fixup)(struct cvm_mmc_host *, struct mmc_command *,
			   struct mmc_data *, u64);
	void (*dmar_fixup_done)(struct cvm_mmc_host *);
};

struct cvm_mmc_slot {
	struct mmc_host *mmc;		/* slot-level mmc_core object */
	struct cvm_mmc_host *host;	/* common hw for all slots */

	u64 clock;

	u64 cached_switch;
	u64 cached_rca;

	bool tuned;
	u64 taps;			/* otx2: MIO_EMM_TIMING */
	unsigned int cmd_cnt;		/* otx: sample cmd in delay */
	unsigned int data_cnt;		/* otx: sample data in delay */

	unsigned int drive;		/* Current drive */
	unsigned int slew;		/* clock skew */

	int bus_id;
	bool cmd6_pending;
	u64 want_switch;
};

struct cvm_mmc_cr_type {
	u8 ctype;
	u8 rtype;
};

struct cvm_mmc_cr_mods {
	u8 ctype_xor;
	u8 rtype_xor;
};

/* Bitfield definitions */
#define MIO_EMM_DMA_FIFO_CFG_CLR	BIT_ULL(16)
#define MIO_EMM_DMA_FIFO_CFG_INT_LVL	GENMASK_ULL(12, 8)
#define MIO_EMM_DMA_FIFO_CFG_COUNT	GENMASK_ULL(4, 0)

#define MIO_EMM_DMA_FIFO_CMD_RW		BIT_ULL(62)
#define MIO_EMM_DMA_FIFO_CMD_INTDIS	BIT_ULL(60)
#define MIO_EMM_DMA_FIFO_CMD_SWAP32	BIT_ULL(59)
#define MIO_EMM_DMA_FIFO_CMD_SWAP16	BIT_ULL(58)
#define MIO_EMM_DMA_FIFO_CMD_SWAP8	BIT_ULL(57)
#define MIO_EMM_DMA_FIFO_CMD_ENDIAN	BIT_ULL(56)
#define MIO_EMM_DMA_FIFO_CMD_SIZE	GENMASK_ULL(55, 36)

#define MIO_EMM_CMD_SKIP_BUSY		BIT_ULL(62)
#define MIO_EMM_CMD_BUS_ID		GENMASK_ULL(61, 60)
#define MIO_EMM_CMD_VAL			BIT_ULL(59)
#define MIO_EMM_CMD_DBUF		BIT_ULL(55)
#define MIO_EMM_CMD_OFFSET		GENMASK_ULL(54, 49)
#define MIO_EMM_CMD_CTYPE_XOR		GENMASK_ULL(42, 41)
#define MIO_EMM_CMD_RTYPE_XOR		GENMASK_ULL(40, 38)
#define MIO_EMM_CMD_IDX			GENMASK_ULL(37, 32)
#define MIO_EMM_CMD_ARG			GENMASK_ULL(31, 0)

#define MIO_EMM_DMA_SKIP_BUSY		BIT_ULL(62)
#define MIO_EMM_DMA_BUS_ID		GENMASK_ULL(61, 60)
#define MIO_EMM_DMA_VAL			BIT_ULL(59)
#define MIO_EMM_DMA_SECTOR		BIT_ULL(58)
#define MIO_EMM_DMA_DAT_NULL		BIT_ULL(57)
#define MIO_EMM_DMA_THRES		GENMASK_ULL(56, 51)
#define MIO_EMM_DMA_REL_WR		BIT_ULL(50)
#define MIO_EMM_DMA_RW			BIT_ULL(49)
#define MIO_EMM_DMA_MULTI		BIT_ULL(48)
#define MIO_EMM_DMA_BLOCK_CNT		GENMASK_ULL(47, 32)
#define MIO_EMM_DMA_CARD_ADDR		GENMASK_ULL(31, 0)

#define MIO_EMM_DMA_CFG_EN		BIT_ULL(63)
#define MIO_EMM_DMA_CFG_RW		BIT_ULL(62)
#define MIO_EMM_DMA_CFG_CLR		BIT_ULL(61)
#define MIO_EMM_DMA_CFG_SWAP32		BIT_ULL(59)
#define MIO_EMM_DMA_CFG_SWAP16		BIT_ULL(58)
#define MIO_EMM_DMA_CFG_SWAP8		BIT_ULL(57)
#define MIO_EMM_DMA_CFG_ENDIAN		BIT_ULL(56)
#define MIO_EMM_DMA_CFG_SIZE		GENMASK_ULL(55, 36)
#define MIO_EMM_DMA_CFG_ADR		GENMASK_ULL(35, 0)

#define MIO_EMM_CFG_BUS_ENA		GENMASK_ULL(3, 0)

#define MIO_EMM_IO_CTL_DRIVE		GENMASK_ULL(3, 2)
#define MIO_EMM_IO_CTL_SLEW		BIT_ULL(0)

#define MIO_EMM_CALB_START		BIT_ULL(0)
#define MIO_EMM_TAP_DELAY		GENMASK_ULL(7, 0)

#define MIO_EMM_TIMING_CMD_IN		GENMASK_ULL(53, 48)
#define MIO_EMM_TIMING_CMD_OUT		GENMASK_ULL(37, 32)
#define MIO_EMM_TIMING_DATA_IN		GENMASK_ULL(21, 16)
#define MIO_EMM_TIMING_DATA_OUT		GENMASK_ULL(5, 0)

#define MIO_EMM_INT_NCB_RAS		BIT_ULL(8)
#define MIO_EMM_INT_NCB_FLT		BIT_ULL(7)
#define MIO_EMM_INT_SWITCH_ERR		BIT_ULL(6)
#define MIO_EMM_INT_SWITCH_DONE		BIT_ULL(5)
#define MIO_EMM_INT_DMA_ERR		BIT_ULL(4)
#define MIO_EMM_INT_CMD_ERR		BIT_ULL(3)
#define MIO_EMM_INT_DMA_DONE		BIT_ULL(2)
#define MIO_EMM_INT_CMD_DONE		BIT_ULL(1)
#define MIO_EMM_INT_BUF_DONE		BIT_ULL(0)

#define MIO_EMM_RSP_STS_BUS_ID		GENMASK_ULL(61, 60)
#define MIO_EMM_RSP_STS_CMD_VAL		BIT_ULL(59)
#define MIO_EMM_RSP_STS_SWITCH_VAL	BIT_ULL(58)
#define MIO_EMM_RSP_STS_DMA_VAL		BIT_ULL(57)
#define MIO_EMM_RSP_STS_DMA_PEND	BIT_ULL(56)
#define MIO_EMM_RSP_STS_DBUF_ERR	BIT_ULL(28)
#define MIO_EMM_RSP_STS_DBUF		BIT_ULL(23)
#define MIO_EMM_RSP_STS_BLK_TIMEOUT	BIT_ULL(22)
#define MIO_EMM_RSP_STS_BLK_CRC_ERR	BIT_ULL(21)
#define MIO_EMM_RSP_STS_RSP_BUSYBIT	BIT_ULL(20)
#define MIO_EMM_RSP_STS_STP_TIMEOUT	BIT_ULL(19)
#define MIO_EMM_RSP_STS_STP_CRC_ERR	BIT_ULL(18)
#define MIO_EMM_RSP_STS_STP_BAD_STS	BIT_ULL(17)
#define MIO_EMM_RSP_STS_STP_VAL		BIT_ULL(16)
#define MIO_EMM_RSP_STS_RSP_TIMEOUT	BIT_ULL(15)
#define MIO_EMM_RSP_STS_RSP_CRC_ERR	BIT_ULL(14)
#define MIO_EMM_RSP_STS_RSP_BAD_STS	BIT_ULL(13)
#define MIO_EMM_RSP_STS_RSP_VAL		BIT_ULL(12)
#define MIO_EMM_RSP_STS_RSP_TYPE	GENMASK_ULL(11, 9)
#define MIO_EMM_RSP_STS_CMD_TYPE	GENMASK_ULL(8, 7)
#define MIO_EMM_RSP_STS_CMD_IDX		GENMASK_ULL(6, 1)
#define MIO_EMM_RSP_STS_CMD_DONE	BIT_ULL(0)

#define MIO_EMM_SAMPLE_CMD_CNT		GENMASK_ULL(25, 16)
#define MIO_EMM_SAMPLE_DAT_CNT		GENMASK_ULL(9, 0)

#define MIO_EMM_SWITCH_BUS_ID		GENMASK_ULL(61, 60)
#define MIO_EMM_SWITCH_EXE		BIT_ULL(59)
#define MIO_EMM_SWITCH_ERR0		BIT_ULL(58)
#define MIO_EMM_SWITCH_ERR1		BIT_ULL(57)
#define MIO_EMM_SWITCH_ERR2		BIT_ULL(56)
#define MIO_EMM_SWITCH_ERRS		GENMASK_ULL(58, 56)
#define MIO_EMM_SWITCH_HS400_TIMING	BIT_ULL(50)
#define MIO_EMM_SWITCH_HS200_TIMING	BIT_ULL(49)
#define MIO_EMM_SWITCH_HS_TIMING	BIT_ULL(48)
#define MIO_EMM_SWITCH_TIMING		GENMASK_ULL(50, 48)
#define MIO_EMM_SWITCH_BUS_WIDTH	GENMASK_ULL(42, 40)
#define MIO_EMM_SWITCH_POWER_CLASS	GENMASK_ULL(35, 32)
#define MIO_EMM_SWITCH_CLK		GENMASK_ULL(31, 0)
#define MIO_EMM_SWITCH_CLK_HI		GENMASK_ULL(31, 16)
#define MIO_EMM_SWITCH_CLK_LO		GENMASK_ULL(15, 0)

/* Protoypes */
irqreturn_t cvm_mmc_interrupt(int irq, void *dev_id);
int cvm_mmc_of_slot_probe(struct device *dev, struct cvm_mmc_host *host);
int cvm_mmc_of_slot_remove(struct cvm_mmc_slot *slot);
void calibrate_mmc(struct cvm_mmc_host *host);

extern const char *cvm_mmc_irq_names[];

static inline bool is_mmc_8xxx(struct cvm_mmc_host *host)
{
	struct pci_dev *pdev = host->pdev;
	u32 chip_id = (pdev->subsystem_device >> 12) & 0xF;

	return (chip_id == PCI_SUBSYS_DEVID_8XXX);
}

static inline bool is_mmc_otx2(struct cvm_mmc_host *host)
{
	struct pci_dev *pdev = host->pdev;
	u32 chip_id = (pdev->subsystem_device >> 12) & 0xF;

	return (chip_id == PCI_SUBSYS_DEVID_9XXX);
}

static inline bool is_mmc_otx2_A0(struct cvm_mmc_host *host)
{
	struct pci_dev *pdev = host->pdev;
	u32 chip_id = (pdev->subsystem_device >> 12) & 0xF;

	return (pdev->revision == 0x00) &&
		(chip_id == PCI_SUBSYS_DEVID_9XXX);
}
#endif

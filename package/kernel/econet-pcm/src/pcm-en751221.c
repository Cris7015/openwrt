// SPDX-License-Identifier: GPL-2.0
/*
 * EcoNet/TrendChip EN751221 PCM/TDM controller driver.
 *
 * Phase 1: probe, map the register block, verify the SoC, soft-reset the PCM
 * engine, load the reset-default register values and keep all interrupts
 * masked. TX/RX descriptor rings, the char/ALSA interface and the SLIC glue
 * are added in later phases.
 *
 * Register sequences were reconstructed from the OEM Trendchip 2.6.36 source
 * (pcmdriver.h) and disassembly of the stock pcm1.ko.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/irqdomain.h>

#include "pcm-en751221.h"

struct pcm_dev {
	struct device *dev;
	void __iomem *base;	/* PCM register block (DT reg) */
	void __iomem *scu;	/* SoC SCU @ 0x1fb00000 (chip-id + reset) */
	void __iomem *chip_scu;	/* Chip SCU @ 0x1fa20000 (PCM pinmux/clock) */
	int irq;
	struct dentry *dbg;
	struct mutex selftest_lock;

	/* DMA descriptor rings (phase 2): PCM_DESC_NUM x struct pcm_desc each */
	struct pcm_desc *tx_ring;
	struct pcm_desc *rx_ring;
	dma_addr_t tx_dma;
	dma_addr_t rx_dma;
};

static inline u32 pcm_rd(struct pcm_dev *p, enum pcm_reg r)
{
	return readl(p->base + PCM_REG(r));
}

static inline void pcm_wr(struct pcm_dev *p, enum pcm_reg r, u32 v)
{
	writel(v, p->base + PCM_REG(r));
}

/* Reset-default register values (OEM enum pcm_reg_num comments). */
static const u32 pcm_reg_defaults[PCM_REG_COUNT] = {
	[PCM_INTFACE_CTRL]		= 0x0100040a,
	[PCM_TX_TIME_SLOT_CFG0]		= 0x00080000,
	[PCM_TX_TIME_SLOT_CFG1]		= 0x00180010,
	[PCM_TX_TIME_SLOT_CFG2]		= 0x00280020,
	[PCM_TX_TIME_SLOT_CFG3]		= 0x00380030,
	[PCM_RX_TIME_SLOT_CFG0]		= 0x00080000,
	[PCM_RX_TIME_SLOT_CFG1]		= 0x00180010,
	[PCM_RX_TIME_SLOT_CFG2]		= 0x00280020,
	[PCM_RX_TIME_SLOT_CFG3]		= 0x00380030,
	[PCM_TX_RX_DESC_RING_SIZE_OFFSET] = 0x000000c0,
	[PCM_TX_RX_DMA_CTRL]		= 0x0f000000,
	/* ISR/IMR/polling/desc-ring-base default to 0 */
};

/*
 * Soft-reset the PCM0 engine via the SCU. Reconstructed from softReset() in
 * pcm1.ko: toggle bit 0x800 of SCU+0x834 around a short delay.
 */
static void pcm_soft_reset(struct pcm_dev *p)
{
	u32 v = readl(p->scu + SCU_PCM_RESET);

	writel(v | SCU_PCM0_RESET_BIT, p->scu + SCU_PCM_RESET);
	usleep_range(1000, 2000);
	writel(v & ~SCU_PCM0_RESET_BIT, p->scu + SCU_PCM_RESET);
	usleep_range(1000, 2000);
}

static void pcm_load_defaults(struct pcm_dev *p)
{
	int i;

	for (i = 0; i < PCM_REG_COUNT; i++)
		pcm_wr(p, i, pcm_reg_defaults[i]);

	/* keep all sources masked until the descriptor rings exist (phase 2) */
	pcm_wr(p, PCM_IMR, 0);
	pcm_wr(p, PCM_ISR, pcm_rd(p, PCM_ISR));	/* W1C any pending */
}

/*
 * Minimum PCM clock/pinmux sequence from pcm1.ko init_module():
 *   0x434..0x440: CHIP_SCU+0x104 |= 0x2000
 *   0x444..0x44c: CHIP_SCU+0x0d8 |= 0x1
 *   0x450..0x458: CHIP_SCU+0x148 |= 0x0c
 *   0x474..0x480: non-EN7526C additionally writes 0x8 to +0x0d4 and
 *                  0x00a00301 to +0x0d8.
 *
 * BACK_TO_BACK is internal to INTFACE_CTRL bit 25, so external SLIC pads are
 * not needed for data return. The OEM still enables the PCM/ZSI mux and clock
 * before pcmConfig(); keep that clock sequence for real hardware safety.
 */
static void pcm_enable_clock_iomux(struct pcm_dev *p)
{
	u32 chip = readl(p->scu + SCU_CHIP_ID) & SCU_CHIP_ID_MASK;
	u32 v;

	v = readl(p->chip_scu + CHIP_SCU_IOMUX_CONTROL1);
	writel(v | CHIP_SCU_GPIO_ZSI_ISI, p->chip_scu + CHIP_SCU_IOMUX_CONTROL1);

	v = readl(p->chip_scu + CHIP_SCU_PCM_CLK_OUTPUT);
	writel(v | CHIP_SCU_PCM_CLK_OUT_EN, p->chip_scu + CHIP_SCU_PCM_CLK_OUTPUT);

	v = readl(p->chip_scu + CHIP_SCU_PCM_CLK_SRC_SEL);
	writel(v | FIELD_PREP(CHIP_SCU_PCM_ZSI_CLK_SRC,
			      CHIP_SCU_PCM_ZSI_CLK_SRC_VAL),
	       p->chip_scu + CHIP_SCU_PCM_CLK_SRC_SEL);

	if (chip != SCU_CHIP_EN7526C) {
		writel(0x00000008, p->chip_scu + CHIP_SCU_PCM_CLK_DIV);
		writel(0x00a00301, p->chip_scu + CHIP_SCU_PCM_CLK_OUTPUT);
	}
}

static u32 pcm_selftest_iface_ctrl(struct pcm_dev *p)
{
	(void)p;

	return 0x87071306;
}

static void pcm_selftest_fill_desc(struct pcm_desc *d, dma_addr_t dma,
				   u32 ch_valid, u32 samples)
{
	u32 offset = 0;
	int ch;

	memset(d, 0, sizeof(*d));
	for (ch = 0; ch < PCM_SELFTEST_CHANS; ch++) {
		d->buf_addr[ch] = (u32)((dma + offset) & PCM_DMA_ADDR_MASK);
		offset += samples * (ch_valid & BIT(ch) ? 2 : 1);
	}

	dma_wmb();
	d->status = PCM_DESC_OWN |
		    FIELD_PREP(PCM_DESC_CH_VALID, ch_valid) |
		    FIELD_PREP(PCM_DESC_SAMPLE_SIZE, samples);
}

/*
 * Minimal polled BACK_TO_BACK self-test.
 *
 * Descriptor ownership follows the OEM flow:
 *   SW sets status bit31=1 before enabling DMA (rxDescSet() 0x12e4,
 *   pcmSend() 0x7c0). HW clears bit31 when the descriptor is complete; ISR
 *   checks cleared ownership with bgez/bltz in pcm_lb.ko .text.pcmIsr 0x98
 *   and 0x1c0.
 */
static int pcm_loopback_selftest(struct pcm_dev *p)
{
	const size_t buf_sz = PCM_SELFTEST_BUF_BYTES;
	static const u32 slot_cfg[4] = {
		0x10101000, 0x10301020, 0x00480040, 0x00580050,
	};
	dma_addr_t tx_dma, rx_dma;
	void *tx_buf, *rx_buf;
	u32 old_imr, old_dma, old_iface, old_ring_size, dma_ctrl, iface;
	u32 old_tx_slots[4], old_rx_slots[4];
	u32 *tx_words, *rx_words;
	int i, ret = 0;

	mutex_lock(&p->selftest_lock);

	tx_buf = dma_alloc_coherent(p->dev, buf_sz, &tx_dma, GFP_KERNEL);
	if (!tx_buf) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	rx_buf = dma_alloc_coherent(p->dev, buf_sz, &rx_dma, GFP_KERNEL);
	if (!rx_buf) {
		ret = -ENOMEM;
		goto out_free_tx;
	}

	if ((tx_dma | rx_dma) & ~PCM_DMA_ADDR_MASK) {
		ret = -EOVERFLOW;
		goto out_free_rx;
	}

	old_imr = pcm_rd(p, PCM_IMR);
	old_dma = pcm_rd(p, PCM_TX_RX_DMA_CTRL);
	old_iface = pcm_rd(p, PCM_INTFACE_CTRL);
	old_ring_size = pcm_rd(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET);
	for (i = 0; i < 4; i++) {
		old_tx_slots[i] = pcm_rd(p, PCM_TX_TIME_SLOT_CFG0 + i);
		old_rx_slots[i] = pcm_rd(p, PCM_RX_TIME_SLOT_CFG0 + i);
	}

	pcm_wr(p, PCM_IMR, 0);
	dma_ctrl = old_dma & ~PCM_DMA_RX_EN;
	pcm_wr(p, PCM_TX_RX_DMA_CTRL, dma_ctrl);
	dma_ctrl &= ~PCM_DMA_TX_EN;
	pcm_wr(p, PCM_TX_RX_DMA_CTRL, dma_ctrl);
	pcm_wr(p, PCM_ISR, pcm_rd(p, PCM_ISR));

	memset(p->tx_ring, 0, PCM_DESC_NUM * sizeof(*p->tx_ring));
	memset(p->rx_ring, 0, PCM_DESC_NUM * sizeof(*p->rx_ring));
	memset(rx_buf, 0, buf_sz);

	tx_words = tx_buf;
	for (i = 0; i < buf_sz / sizeof(u32); i++)
		tx_words[i] = PCM_LOOPBACK_HEADER_PATTERN;

	pcm_enable_clock_iomux(p);
	pcm_wr(p, PCM_TX_DESC_RING_BASE, (u32)(p->tx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x0000009f);
	for (i = 0; i < 4; i++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + i, slot_cfg[i]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + i, slot_cfg[i]);
	}

	iface = pcm_selftest_iface_ctrl(p);
	pcm_wr(p, PCM_INTFACE_CTRL, iface);
	pcm_wr(p, PCM_INTFACE_CTRL, iface & ~BIT(26));
	usleep_range(5000, 6000);
	pcm_wr(p, PCM_INTFACE_CTRL, iface);
	pcm_wr(p, PCM_IMR, 0x00000028);

	dma_ctrl = old_dma & ~(PCM_DMA_CH_VALID_MASK | PCM_DMA_TX_EN |
			       PCM_DMA_RX_EN);
	dma_ctrl |= FIELD_PREP(PCM_DMA_CH_VALID_MASK, PCM_SELFTEST_CH_VALID);
	pcm_wr(p, PCM_TX_RX_DMA_CTRL, dma_ctrl);

	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL, dma_ctrl | PCM_DMA_TX_EN);
	pcm_selftest_fill_desc(&p->rx_ring[0], rx_dma, PCM_SELFTEST_CH_VALID,
			       PCM_SELFTEST_SAMPLES);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       dma_ctrl | PCM_DMA_TX_EN | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);
	pcm_selftest_fill_desc(&p->tx_ring[0], tx_dma, PCM_SELFTEST_CH_VALID,
			       PCM_SELFTEST_SAMPLES);
	dma_wmb();
	pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);

	for (i = 0; i < 500; i++) {
		dma_rmb();
		if (!(READ_ONCE(p->rx_ring[0].status) & PCM_DESC_OWN))
			break;
		usleep_range(1000, 2000);
	}
	if (i == 500) {
		ret = -ETIMEDOUT;
		goto out_restore;
	}

	dma_rmb();
	rx_words = rx_buf;
	for (i = 0; i < buf_sz / sizeof(u32); i++) {
		if (rx_words[i] != PCM_LOOPBACK_HEADER_PATTERN) {
			ret = -EIO;
			break;
		}
	}

out_restore:
	dma_rmb();
	dev_info(p->dev,
		 "loopback diag: ret=%d isr=0x%08x dma_ctrl=0x%08x txdesc=0x%08x rxdesc=0x%08x rx[0]=0x%08x\n",
		 ret, pcm_rd(p, PCM_ISR), pcm_rd(p, PCM_TX_RX_DMA_CTRL),
		 p->tx_ring[0].status, p->rx_ring[0].status, ((u32 *)rx_buf)[0]);
	pcm_wr(p, PCM_TX_RX_DMA_CTRL, old_dma & ~(PCM_DMA_TX_EN | PCM_DMA_RX_EN));
	pcm_wr(p, PCM_INTFACE_CTRL, old_iface);
	for (i = 0; i < 4; i++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + i, old_tx_slots[i]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + i, old_rx_slots[i]);
	}
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, old_ring_size);
	pcm_wr(p, PCM_IMR, old_imr);
	pcm_wr(p, PCM_ISR, pcm_rd(p, PCM_ISR));
	memset(p->tx_ring, 0, PCM_DESC_NUM * sizeof(*p->tx_ring));
	memset(p->rx_ring, 0, PCM_DESC_NUM * sizeof(*p->rx_ring));

out_free_rx:
	dma_free_coherent(p->dev, buf_sz, rx_buf, rx_dma);
out_free_tx:
	dma_free_coherent(p->dev, buf_sz, tx_buf, tx_dma);
out_unlock:
	mutex_unlock(&p->selftest_lock);
	return ret;
}

/*
 * Allocate the TX/RX DMA descriptor rings and program their physical base
 * addresses. From descInit() in pcm1.ko: each ring is PCM_DESC_NUM entries of
 * sizeof(struct pcm_desc) (36 B -> 540 B), zeroed, base written (as a physical
 * address) to PCM_TX/RX_DESC_RING_BASE. The rings are not yet armed or the DMA
 * enabled here - that is phase 2b.
 */
static int pcm_alloc_rings(struct pcm_dev *p)
{
	size_t sz = PCM_DESC_NUM * sizeof(struct pcm_desc);

	p->tx_ring = dmam_alloc_coherent(p->dev, sz, &p->tx_dma, GFP_KERNEL);
	if (!p->tx_ring)
		return -ENOMEM;
	p->rx_ring = dmam_alloc_coherent(p->dev, sz, &p->rx_dma, GFP_KERNEL);
	if (!p->rx_ring)
		return -ENOMEM;

	memset(p->tx_ring, 0, sz);
	memset(p->rx_ring, 0, sz);

	pcm_wr(p, PCM_TX_DESC_RING_BASE, (u32)p->tx_dma);
	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)p->rx_dma);

	dev_info(p->dev, "DMA rings: tx=%pad rx=%pad (%u desc x %zu B)\n",
		 &p->tx_dma, &p->rx_dma, PCM_DESC_NUM, sizeof(struct pcm_desc));
	return 0;
}

/*
 * Exported for the SLIC (ZSI) driver: bring the PCM engine up so it generates
 * continuous PCLK/FSYNC on the external pins, which a ZSI SLIC clocks off.
 *
 * The OEM stock device (voip running) shows the PCM with DMA TX+RX enabled
 * (DMA_CTRL=0x0f000003), INTFACE_CTRL=0xf5071306 and a specific timeslot table;
 * the bit clock only free-runs once DMA is armed. So we arm all descriptors
 * (with dummy buffers), program the caller's timeslots + INTFACE_CTRL, and
 * enable DMA. Left running (not torn down) so the SLIC keeps its clock.
 */
static struct pcm_dev *g_pcm;

int pcm_en751221_zsi_clock_run(u32 intface, const u32 *tx_slots,
			       const u32 *rx_slots)
{
	struct pcm_dev *p = g_pcm;
	static void *txb, *rxb;
	static dma_addr_t txb_dma, rxb_dma;
	const u32 samples = 80, chv = 0xf;
	int i, ch;

	if (!p)
		return -ENODEV;

	if (!txb) {
		txb = dmam_alloc_coherent(p->dev, 1024, &txb_dma, GFP_KERNEL);
		rxb = dmam_alloc_coherent(p->dev, 1024, &rxb_dma, GFP_KERNEL);
		if (!txb || !rxb)
			return -ENOMEM;
	}

	for (i = 0; i < PCM_DESC_NUM; i++) {
		memset(&p->tx_ring[i], 0, sizeof(struct pcm_desc));
		memset(&p->rx_ring[i], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < 4; ch++) {
			p->tx_ring[i].buf_addr[ch] =
				(u32)((txb_dma + ch * 160) & PCM_DMA_ADDR_MASK);
			p->rx_ring[i].buf_addr[ch] =
				(u32)((rxb_dma + ch * 160) & PCM_DMA_ADDR_MASK);
		}
		p->tx_ring[i].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, chv) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, samples);
		p->rx_ring[i].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, chv) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, samples);
	}

	for (i = 0; i < 4; i++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + i, tx_slots[i]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + i, rx_slots[i]);
	}
	pcm_wr(p, PCM_TX_DESC_RING_BASE, (u32)(p->tx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, intface);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, chv) |
	       PCM_DMA_TX_EN | PCM_DMA_RX_EN);
	/* kick the DMA engine -- without the polling demand it never starts
	 * (this is what the 2b loopback self-test needed; ISR stayed 0 without it) */
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);
	pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);

	dev_info(p->dev, "ZSI clock run: intface=0x%08x dma_ctrl=0x%08x isr=0x%08x\n",
		 intface, pcm_rd(p, PCM_TX_RX_DMA_CTRL), pcm_rd(p, PCM_ISR));
	return 0;
}
EXPORT_SYMBOL_GPL(pcm_en751221_zsi_clock_run);

/*
 * Capture one ~10ms voice frame from the SLIC over the PCM bus. The SLIC puts
 * its mic audio on timeslot 6 (SLIC TXSLOT=6); the SoC PCM RX DMA grabs it.
 * RX timeslot cfg3 selects slot 6 (0x00380030), descriptor chValid=0x40 (ch6),
 * sampleSize=80 (80 samples = 10ms @ 8kHz u-law). Copies the bytes to `out`.
 * Used by the SLIC driver to prove audio capture works.
 */
int pcm_en751221_capture_rx(u8 *out, int nbytes)
{
	struct pcm_dev *p = g_pcm;
	static void *cap;
	static dma_addr_t cap_dma;
	int i;

	if (!p)
		return -ENODEV;
	if (nbytes > 80)
		nbytes = 80;

	if (!cap) {
		cap = dmam_alloc_coherent(p->dev, 256, &cap_dma, GFP_KERNEL);
		if (!cap)
			return -ENOMEM;
	}
	memset(cap, 0, 256);

	/* RX timeslots: defaults + slot 6 in CFG3 */
	pcm_wr(p, PCM_RX_TIME_SLOT_CFG0, 0x00080000);
	pcm_wr(p, PCM_RX_TIME_SLOT_CFG1, 0x00180010);
	pcm_wr(p, PCM_RX_TIME_SLOT_CFG2, 0x00280020);
	pcm_wr(p, PCM_RX_TIME_SLOT_CFG3, 0x00380030);

	/* arm RX descriptor 0: own | chValid ch6 (0x40) | 80 samples.
	 * The HW writes channel N's data to buf_addr[N] (see 2b loopback), so the
	 * channel-6 (slot 6 = SLIC mic) buffer goes in buf_addr[6], NOT [0]. */
	memset(p->rx_ring, 0, PCM_DESC_NUM * sizeof(struct pcm_desc));
	p->rx_ring[0].buf_addr[6] = (u32)(cap_dma & PCM_DMA_ADDR_MASK);
	p->rx_ring[0].status = PCM_DESC_OWN |
		FIELD_PREP(PCM_DESC_CH_VALID, 0x40) |
		FIELD_PREP(PCM_DESC_SAMPLE_SIZE, 80);

	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, 0xf5071306);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, 0x40) | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);

	for (i = 0; i < 500; i++) {
		dma_rmb();
		if (!(READ_ONCE(p->rx_ring[0].status) & PCM_DESC_OWN))
			break;
		usleep_range(1000, 2000);
	}
	dma_rmb();
	memcpy(out, cap, nbytes);
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~PCM_DMA_RX_EN);
	return (i < 500) ? 0 : -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(pcm_en751221_capture_rx);

/*
 * Diagnostic: capture ALL 8 RX channels at once (chValid 0xff, OEM timeslot
 * map) into out[8*80]. Lets us scan which channel/slot the SLIC mic audio
 * actually lands on. Does NOT touch the SLIC -- assumes line already set up.
 */
int pcm_en751221_capture_allch(u8 *out)
{
	struct pcm_dev *p = g_pcm;
	static const u32 oem_slots[4] = {
		0x10301020, 0x10501040, 0x10701060, 0x10901080,
	};
	static void *cap;
	static dma_addr_t cap_dma;
	int i, ch, d, done = -1;

	if (!p)
		return -ENODEV;
	/* one 8ch*80 slab per descriptor so the engine can land on any of them */
	if (!cap) {
		cap = dmam_alloc_coherent(p->dev, PCM_DESC_NUM * 8 * 80 + 64,
					  &cap_dma, GFP_KERNEL);
		if (!cap)
			return -ENOMEM;
	}
	memset(cap, 0, PCM_DESC_NUM * 8 * 80);

	for (i = 0; i < 4; i++)
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + i, oem_slots[i]);

	/*
	 * Arm ALL descriptors valid (OWN + 8ch + own buffer slab). After a
	 * previous capture the DMA engine's internal descriptor pointer is
	 * left advanced; re-writing RING_BASE does not always rewind it, so if
	 * only desc0 were valid the engine could resume on a non-OWN descriptor
	 * and stall forever (-ETIMEDOUT on every call after the first). With
	 * every descriptor valid the engine completes wherever it resumes and
	 * we read back whichever one it filled.
	 */
	for (d = 0; d < PCM_DESC_NUM; d++) {
		memset(&p->rx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < 8; ch++)
			p->rx_ring[d].buf_addr[ch] =
				(u32)((cap_dma + (d * 8 + ch) * 80) &
				      PCM_DMA_ADDR_MASK);
		p->rx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, 80);
	}

	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, 0xf5071306);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, 0xff) | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);

	for (i = 0; i < 500; i++) {
		dma_rmb();
		for (d = 0; d < PCM_DESC_NUM; d++) {
			if (!(READ_ONCE(p->rx_ring[d].status) & PCM_DESC_OWN)) {
				done = d;
				break;
			}
		}
		if (done >= 0)
			break;
		usleep_range(1000, 2000);
	}
	dma_rmb();
	if (done < 0)
		done = 0;
	memcpy(out, (u8 *)cap + done * 8 * 80, 8 * 80);
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~PCM_DMA_RX_EN);
	return (i < 500) ? 0 : -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(pcm_en751221_capture_allch);

/* PCM frame geometry: the codec is 16-bit linear, so sample_size counts
 * 16-bit samples and each one occupies 2 bytes on the bus (confirmed by the
 * OEM CODEC_LINEAR setting and the pcm_selftest_fill_desc 2-byte stride). */
#define PCM_SAMP_PER_FRAME	80
#define PCM_BYTES_PER_FRAME	(PCM_SAMP_PER_FRAME * 2)

/*
 * Build a recognisable square-wave melody ("Twinkle Twinkle") as 8 kHz signed
 * 16-bit linear samples, big-endian (the SoC is MIPS BE and the SLIC codec is
 * linear). Square wave (chiptune) so it synthesises with pure integer math.
 * Returns a kmalloc'd byte buffer (caller kfree's), length rounded to a frame.
 */
static u8 *pcm_build_melody(int *out_len)
{
	static const u16 notes[][2] = {	/* { freq Hz (0 = rest), ms } */
		{ 523, 280 }, { 523, 280 }, { 784, 280 }, { 784, 280 },
		{ 880, 280 }, { 880, 280 }, { 784, 520 }, {   0,  60 },
		{ 698, 280 }, { 698, 280 }, { 659, 280 }, { 659, 280 },
		{ 587, 280 }, { 587, 280 }, { 523, 520 },
	};
	int n = ARRAY_SIZE(notes), i, j, idx = 0, total = 0;
	u16 phase = 0;
	u8 *buf;

	for (i = 0; i < n; i++)
		total += notes[i][1] * 8;	/* 8 samples per ms @ 8 kHz */
	buf = kmalloc(total * 2 + PCM_BYTES_PER_FRAME, GFP_KERNEL);
	if (!buf)
		return NULL;

	for (i = 0; i < n; i++) {
		u16 f = notes[i][0];
		int samp = notes[i][1] * 8;
		u32 inc = f ? (u32)f * 65536 / 8000 : 0;

		for (j = 0; j < samp; j++) {
			s16 s = 0;

			if (f) {
				phase += inc;
				s = (phase & 0x8000) ? 20000 : -20000;
			}
			buf[idx++] = (u8)(s >> 8);	/* big-endian */
			buf[idx++] = (u8)(s & 0xff);
		}
	}
	while (idx % PCM_BYTES_PER_FRAME) {	/* pad final frame with silence */
		buf[idx++] = 0;
		buf[idx++] = 0;
	}
	*out_len = idx;
	return buf;
}

/*
 * Play the melody out the PCM TX path, broadcast to all 8 channels (chValid
 * 0xff) so it lands on whichever bus slot the SLIC earpiece (RXSLOT) reads.
 * Software-recycles the 15-descriptor ring for the whole tune. Blocks for the
 * duration of the melody (~4 s); meant to be driven from a debugfs read.
 */
int pcm_en751221_play_melody(void)
{
	struct pcm_dev *p = g_pcm;
	static const u32 oem_slots[4] = {
		0x10301020, 0x10501040, 0x10701060, 0x10901080,
	};
	static void *txb, *rxb;
	static dma_addr_t txb_dma, rxb_dma;
	u8 *mel;
	const u32 ch_valid = 0x0f;
	int mel_len = 0, pos = 0, d, ch, guard = 0;

	if (!p)
		return -ENODEV;
	if (!txb) {
		txb = dmam_alloc_coherent(p->dev,
					  PCM_DESC_NUM * PCM_BYTES_PER_FRAME + 64,
					  &txb_dma, GFP_KERNEL);
		rxb = dmam_alloc_coherent(p->dev, 4 * PCM_BYTES_PER_FRAME + 64,
					  &rxb_dma, GFP_KERNEL);
		if (!txb || !rxb)
			return -ENOMEM;
	}
	mel = pcm_build_melody(&mel_len);
	if (!mel)
		return -ENOMEM;

	for (d = 0; d < 4; d++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + d, oem_slots[d]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + d, oem_slots[d]);
	}
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~(PCM_DMA_TX_EN | PCM_DMA_RX_EN));
	pcm_wr(p, PCM_ISR, pcm_rd(p, PCM_ISR));

	/* prime the TX ring with the first PCM_DESC_NUM frames (160 B each) */
	for (d = 0; d < PCM_DESC_NUM && pos < mel_len; d++) {
		memset(&p->tx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < 4; ch++)
			p->tx_ring[d].buf_addr[ch] =
				(u32)((txb_dma + d * PCM_BYTES_PER_FRAME) &
				      PCM_DMA_ADDR_MASK);
		memcpy((u8 *)txb + d * PCM_BYTES_PER_FRAME, mel + pos,
		       PCM_BYTES_PER_FRAME);
		pos += PCM_BYTES_PER_FRAME;
		p->tx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, ch_valid) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
	}

	/*
	 * The full-duplex frame engine only advances the TX descriptors when
	 * RX is also enabled (TX-only leaves OWN bits set forever -- ISR shows
	 * frame boundaries but no TX descriptor updates). Arm a throwaway RX
	 * ring into a scratch buffer so the engine clocks, mirroring the
	 * known-good zsi_clock_run dual-direction setup.
	 */
	for (d = 0; d < PCM_DESC_NUM; d++) {
		memset(&p->rx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < 4; ch++)
			p->rx_ring[d].buf_addr[ch] =
				(u32)((rxb_dma + ch * PCM_BYTES_PER_FRAME) &
				      PCM_DMA_ADDR_MASK);
		p->rx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, ch_valid) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
	}

	pcm_wr(p, PCM_TX_DESC_RING_BASE, (u32)(p->tx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, 0xf5071306);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, ch_valid) |
	       PCM_DMA_TX_EN | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);

	/* refill descriptors as the engine releases them, until the tune ends */
	while (pos < mel_len && guard < 2000) {	/* ~6s no-progress bail-out */
		int progressed = 0;

		for (d = 0; d < PCM_DESC_NUM && pos < mel_len; d++) {
			dma_rmb();
			if (READ_ONCE(p->tx_ring[d].status) & PCM_DESC_OWN)
				continue;
			memcpy((u8 *)txb + d * PCM_BYTES_PER_FRAME, mel + pos,
			       PCM_BYTES_PER_FRAME);
			pos += PCM_BYTES_PER_FRAME;
			dma_wmb();
			p->tx_ring[d].status = PCM_DESC_OWN |
				FIELD_PREP(PCM_DESC_CH_VALID, ch_valid) |
				FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
			pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);
			progressed = 1;
		}
		/* keep the RX side clocking so TX keeps advancing */
		for (d = 0; d < PCM_DESC_NUM; d++) {
			if (READ_ONCE(p->rx_ring[d].status) & PCM_DESC_OWN)
				continue;
			p->rx_ring[d].status = PCM_DESC_OWN |
				FIELD_PREP(PCM_DESC_CH_VALID, ch_valid) |
				FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
		}
		pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);
		if (!progressed) {
			usleep_range(2000, 4000);
			guard++;
		}
	}
	msleep(220);		/* let the last queued frames drain */
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~(PCM_DMA_TX_EN | PCM_DMA_RX_EN));
	kfree(mel);
	return (guard < 2000) ? 0 : -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(pcm_en751221_play_melody);

/*
 * Real-time full-duplex sidetone loopback: continuously capture the mic (RX
 * ch0 = slot 4) and feed it back into the TX ring so it plays out the earpiece
 * (~1 ring / 150 ms delayed). Proves continuous full-duplex streaming and
 * exercises the descriptor-recycle logic the char device will reuse. Runs for
 * `seconds`, then stops. Blocks.
 */
int pcm_en751221_voice_loopback(int seconds)
{
	struct pcm_dev *p = g_pcm;
	static const u32 oem_slots[4] = {
		0x10301020, 0x10501040, 0x10701060, 0x10901080,
	};
	static void *txb, *rxb;
	static dma_addr_t txb_dma, rxb_dma;
	const u32 ch_valid = 0x0f;
	const int FB = PCM_BYTES_PER_FRAME;	/* 160 bytes/frame/channel */
	unsigned long end;
	int d, ch;

	if (!p)
		return -ENODEV;
	if (!txb) {
		txb = dmam_alloc_coherent(p->dev, PCM_DESC_NUM * FB + 64,
					  &txb_dma, GFP_KERNEL);
		rxb = dmam_alloc_coherent(p->dev, PCM_DESC_NUM * 4 * FB + 64,
					  &rxb_dma, GFP_KERNEL);
		if (!txb || !rxb)
			return -ENOMEM;
	}
	memset(txb, 0, PCM_DESC_NUM * FB);
	memset(rxb, 0, PCM_DESC_NUM * 4 * FB);

	for (d = 0; d < 4; d++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + d, oem_slots[d]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + d, oem_slots[d]);
	}
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~(PCM_DMA_TX_EN | PCM_DMA_RX_EN));
	pcm_wr(p, PCM_ISR, pcm_rd(p, PCM_ISR));

	for (d = 0; d < PCM_DESC_NUM; d++) {
		memset(&p->tx_ring[d], 0, sizeof(struct pcm_desc));
		memset(&p->rx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < 4; ch++) {
			/* TX: all channels share this descriptor's TX frame */
			p->tx_ring[d].buf_addr[ch] =
				(u32)((txb_dma + d * FB) & PCM_DMA_ADDR_MASK);
			/* RX: each channel its own slab (ch0 = mic) */
			p->rx_ring[d].buf_addr[ch] =
				(u32)((rxb_dma + (d * 4 + ch) * FB) &
				      PCM_DMA_ADDR_MASK);
		}
		p->tx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, ch_valid) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
		p->rx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, ch_valid) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
	}

	pcm_wr(p, PCM_TX_DESC_RING_BASE, (u32)(p->tx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, 0xf5071306);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, ch_valid) |
	       PCM_DMA_TX_EN | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);

	end = jiffies + msecs_to_jiffies(seconds * 1000);
	while (time_before(jiffies, end)) {
		int progressed = 0;

		for (d = 0; d < PCM_DESC_NUM; d++) {
			dma_rmb();
			if (!(READ_ONCE(p->rx_ring[d].status) & PCM_DESC_OWN)) {
				/* mic captured (ch0) -> this desc's TX frame */
				memcpy((u8 *)txb + d * FB,
				       (u8 *)rxb + (d * 4 + 0) * FB, FB);
				dma_wmb();
				p->rx_ring[d].status = PCM_DESC_OWN |
					FIELD_PREP(PCM_DESC_CH_VALID, ch_valid) |
					FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
				progressed = 1;
			}
			if (!(READ_ONCE(p->tx_ring[d].status) & PCM_DESC_OWN)) {
				dma_wmb();
				p->tx_ring[d].status = PCM_DESC_OWN |
					FIELD_PREP(PCM_DESC_CH_VALID, ch_valid) |
					FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
				progressed = 1;
			}
		}
		pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);
		pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);
		if (!progressed)
			usleep_range(1000, 2000);
		else
			usleep_range(500, 1000);
	}
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~(PCM_DMA_TX_EN | PCM_DMA_RX_EN));
	return 0;
}
EXPORT_SYMBOL_GPL(pcm_en751221_voice_loopback);

/* ---- Continuous full-duplex voice streaming engine (for the char device) ----
 * Continuous TX+RX DMA using the same 8-slot RX layout as rx_scan2.
 * A kthread recycles descriptors:
 * RX-done -> push mic frame (ch1) into capture FIFO; TX-done -> pull a frame
 * from playback FIFO (or silence) into the TX ring. read()/write() drain/fill
 * the FIFOs. 16-bit linear, 8 kHz, mono. SPSC kfifos (lock-free).
 */
#define VOICE_FB	PCM_BYTES_PER_FRAME	/* 160 B = 80 samples * 2 */
#define VOICE_FIFO_SZ	4096			/* default play/cap kfifo. kfifo rounds
						 * to a power of two; the latency knee
						 * with the clean u-law capture is 4096
						 * (~0.26 s, fluid) -- 2048 chops. Was
						 * 12288 before u-law. Live-tunable via
						 * the voice_fifo_sz param. */
#define VOICE_CHANS	8
#define VOICE_TX_CH_VALID	0x0f	/* 4ch, like play_melody (full-duplex proven) */
#define VOICE_RX_CH_VALID	0x0f	/* 4ch -- play_melody re-arms RX desc fine at
					 * 0x0f, so RX DOES complete; the earlier
					 * read-hang was the voice_thread margin, not
					 * the chNum (fixed below). */
#define VOICE_RX_CH	0		/* mic = ch0 at chNum=4 (scan-confirmed:
					 * clean voice, ~42dB SNR). NB the slot->ch
					 * map shifts with chNum (was ch2 at chNum=8) */

/* RX capture channel, runtime-tunable so the mic channel can be scanned
 * (it shifts with chNum): echo N > /sys/module/pcm_en751221/parameters/voice_rx_ch */
static int voice_rx_ch = VOICE_RX_CH;
module_param(voice_rx_ch, int, 0644);

/* Digital capture gain (mic is captured very quiet). 1 = passthrough.
 * Tune live: echo N > /sys/module/pcm_en751221/parameters/voice_gain */
static int voice_gain = 1;
module_param(voice_gain, int, 0644);

/* Byte position of the 8-bit u-law code in the 16-bit playback (TX) timeslot.
 * The SLIC uses different clock-slot offsets for TX-capture (CLKSLOTS TCS) vs
 * RX-playback (RCS), so the code can land in a different byte than capture.
 * 1 = high byte (MSB), 0 = low byte (LSB). Tune live during a call:
 * echo {0,1} > /sys/module/pcm_en751221/parameters/tx_msb */
static int tx_msb = 1;
module_param(tx_msb, int, 0644);

/* Voice FIFO size (bytes) for the play (earpiece) and capture (mic) kfifos;
 * kfifo rounds it up to a power of two. This is the dominant audio-latency knob:
 * the play path can buffer up to this much (PC->Philips delay), while bigger
 * avoids underrun chops. The old big value predates the clean u-law capture.
 * Tune live (takes effect on the next call/open):
 * echo N > /sys/module/pcm_en751221/parameters/voice_fifo_sz */
static int voice_fifo_sz = VOICE_FIFO_SZ;
module_param(voice_fifo_sz, int, 0644);

/* Servicing mode for the voice thread:
 *   1 = experimental IRQ-driven mode: sleep until the PCM raises a descriptor
 *       completion IRQ, with a 5ms timeout fallback.
 *   0 = stable usleep_range() poller (~0.5-2.5ms cadence).
 *
 * Keep polling as the default. Hardware A/B on the XR500v with Linux 6.18
 * found that IRQ mode can leave the physical PCM stream silent after the first
 * call is closed and the device is reopened, even though SIP/RTP continues at
 * 64 kbit/s and the per-open PCM soft reset runs. Two consecutive calls work
 * in both directions with polling. IRQ mode remains available for diagnosis.
 * Live-tunable for A/B: echo {0,1} > /sys/module/pcm_en751221/parameters/voice_use_irq */
static int voice_use_irq;
module_param(voice_use_irq, int, 0644);

/* Safety poll fallback (ms) when IRQ-driven: the thread wakes at least this often
 * even if no IRQ arrives. The 15-deep ring is ~150ms so this is a wide margin. */
static int voice_irq_timeout_ms = 5;
module_param(voice_irq_timeout_ms, int, 0644);

/* Diagnostic/bring-up override: force the PCM to request a specific intc hwirq
 * instead of the DT 'interrupts' value. -1 = use the DT. Lets us hot-test the
 * real PCM intc line (e.g. 12 vs 13) WITHOUT reflashing the DTB:
 *   insmod pcm-en751221.ko pcm_hwirq=12
 * Mapped through the en751221-intc irq_domain. */
static int pcm_hwirq = -1;
module_param(pcm_hwirq, int, 0444);

/* TX mute flag, set by the SLIC hook poller via pcm_en751221_voice_set_mute().
 * When the line goes on-hook mid-call the analog loop opens and the capture path
 * picks up a loud dead-line transient; muting here feeds silence to the peer in
 * ~one frame instead of waiting ~450ms for the callmgr to debounce + hang up.
 * Reset to 0 in voice_start so a new call never starts muted. */
static int voice_tx_mute;

static struct {
	bool active;
	void *txb, *rxb;
	dma_addr_t txb_dma, rxb_dma;
	struct task_struct *thr;
	struct kfifo cap;	/* mic -> userspace (read) */
	struct kfifo play;	/* userspace (write) -> earpiece */
	wait_queue_head_t cap_wq, play_wq;
	wait_queue_head_t irq_wq;	/* woken by pcm_irq on descriptor completion */
	int irq_pending;		/* set by pcm_irq, cleared by the voice thread */
} vs;

/*
 * G.711 u-law codec. The Le9642 PCM interface drives (capture) and reads
 * (playback) an 8-bit u-law code in the LOW byte of each 16-bit timeslot --
 * the chip emits only 8 bits per slot even in "linear" mode, so we run it in
 * u-law and (de)compand in software. u-law's logarithmic companding preserves
 * quiet speech that 8-bit linear would quantize to silence (the "choppy"
 * capture). The char device stays 16-bit linear so baresip is unchanged.
 */
static inline s16 ulaw_decode(u8 byte)
{
	int u = (~byte) & 0xff;
	int sign = u & 0x80;
	int exp = (u >> 4) & 0x07;
	int man = u & 0x0f;
	int s = (((man << 3) + 0x84) << exp) - 0x84;

	return sign ? -s : s;
}

static inline u8 ulaw_encode(s16 pcm)
{
	int sign = (pcm >> 8) & 0x80;
	int mag = sign ? -(int)pcm : (int)pcm;
	int exp, man;

	if (mag > 32635)
		mag = 32635;
	mag += 0x84;
	exp = fls(mag >> 7) - 1;
	if (exp > 7)
		exp = 7;
	else if (exp < 0)
		exp = 0;
	man = (mag >> (exp + 3)) & 0x0f;
	return ~(sign | (exp << 4) | man);
}

static int pcm_voice_thread(void *data)
{
	struct pcm_dev *p = g_pcm;
	u8 frame[VOICE_FB];
	int rx_head = 0, tx_head = 0;

	while (!kthread_should_stop()) {
		int progressed = 0;

		/*
		 * RX in strict completion order (same structure as the TX loop):
		 * drain every descriptor whose OWN bit the DMA has cleared, copy
		 * the mic channel into the capture fifo, then re-arm it. The earlier
		 * 2-descriptor "margin" (requiring rx_head AND rx_head+1 done) could
		 * wedge and never fill the fifo -> read() blocked forever. play_melody
		 * proves a plain OWN check advances RX fine at this chNum.
		 */
		while (!(READ_ONCE(p->rx_ring[rx_head].status) & PCM_DESC_OWN)) {
			dma_rmb();
			if (kfifo_avail(&vs.cap) >= VOICE_FB) {
				s16 *src = (s16 *)((u8 *)vs.rxb +
					(rx_head * VOICE_CHANS + voice_rx_ch) * VOICE_FB);
				s16 tmp[PCM_SAMP_PER_FRAME];
				int i;

				/* The SLIC drives an 8-bit u-law code in the low
				 * byte of each timeslot; decode to 16-bit linear. */
				for (i = 0; i < PCM_SAMP_PER_FRAME; i++) {
					int x = ulaw_decode(src[i] & 0xff);

					if (voice_gain > 1) {
						x *= voice_gain;
						if (x > 32767)
							x = 32767;
						else if (x < -32768)
							x = -32768;
					}
					tmp[i] = (s16)x;
				}
				/* on-hook: feed silence so the dead-line transient
				 * is not streamed to the peer (see voice_tx_mute) */
				if (READ_ONCE(voice_tx_mute))
					memset(tmp, 0, sizeof(tmp));
				kfifo_in(&vs.cap, tmp, VOICE_FB);
			}
			dma_wmb();
			p->rx_ring[rx_head].status = PCM_DESC_OWN |
				FIELD_PREP(PCM_DESC_CH_VALID, VOICE_RX_CH_VALID) |
				FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
			rx_head = (rx_head + 1) % PCM_DESC_NUM;
			progressed = 1;
		}

		/* TX in strict completion order (write buffer before arming, so no
		 * read-during-write concern on the TX side). */
		while (!(READ_ONCE(p->tx_ring[tx_head].status) & PCM_DESC_OWN)) {
			int n = 0;

			if (kfifo_len(&vs.play) >= VOICE_FB)
				n = kfifo_out(&vs.play, frame, VOICE_FB);
			if (n < VOICE_FB)
				memset(frame + n, 0, VOICE_FB - n);
			{
				/* Encode the 16-bit linear playback samples to
				 * 8-bit u-law, placed in the MSB (or LSB) byte of
				 * each timeslot (the other byte 0); the SLIC codec
				 * is u-law. tx_msb selects the byte the SLIC reads. */
				s16 *f = (s16 *)frame;
				int i;

				for (i = 0; i < PCM_SAMP_PER_FRAME; i++) {
					u8 code = ulaw_encode(f[i]);

					f[i] = tx_msb ? (s16)((u16)code << 8) : code;
				}
			}
			memcpy((u8 *)vs.txb + tx_head * VOICE_FB, frame, VOICE_FB);
			dma_wmb();
			p->tx_ring[tx_head].status = PCM_DESC_OWN |
				FIELD_PREP(PCM_DESC_CH_VALID, VOICE_TX_CH_VALID) |
				FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
			tx_head = (tx_head + 1) % PCM_DESC_NUM;
			progressed = 1;
		}

		pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);
		pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);
		if (progressed) {
			wake_up_interruptible(&vs.cap_wq);
			wake_up_interruptible(&vs.play_wq);
		}
		if (READ_ONCE(voice_use_irq)) {
			/* Sleep until the PCM signals a descriptor completed; the
			 * timeout is a safety net so a missed IRQ never wedges audio. */
			int t = READ_ONCE(voice_irq_timeout_ms);

			wait_event_interruptible_timeout(vs.irq_wq,
				READ_ONCE(vs.irq_pending) || kthread_should_stop(),
				msecs_to_jiffies(t > 0 ? t : 5));
			WRITE_ONCE(vs.irq_pending, 0);
		} else if (progressed) {
			usleep_range(500, 1000);
		} else {
			usleep_range(1500, 2500);
		}
	}
	return 0;
}

/*
 * Reset the PCM DMA engine before each call. Disabling/re-enabling the TX/RX
 * EN bits in voice_start does NOT rewind the engine's internal descriptor
 * pointer, so the 2nd and later calls resume mid-ring while the voice thread
 * restarts at descriptor 0 -> they desync, the DMA reaches descriptors the
 * thread has not re-armed (ISR END_OF_TX/RX_DESC latch, reg 0x24 = 0x3d vs a
 * healthy 0x0d) and the ring starves -> the earpiece chops. The 1st call after
 * probe is clean only because probe already soft-reset the engine.
 *
 * This MUST run BEFORE the SLIC line-up: the soft reset blips the TDM clock the
 * SLIC synchronises to, so resetting after line-up desyncs the codec highway
 * (audio stops, RTP goes to 0, the peer drops the call). The char-device open
 * calls this at its very top, ahead of the SLIC reset + profile load.
 */
void pcm_en751221_voice_reset(void)
{
	if (g_pcm) {
		pcm_soft_reset(g_pcm);
		pcm_load_defaults(g_pcm);
	}
}
EXPORT_SYMBOL_GPL(pcm_en751221_voice_reset);

/*
 * Mute/un-mute the captured (TX) audio. Called from the SLIC driver's hook
 * poller: on-hook -> mute (silence to the peer), off-hook -> un-mute. It only
 * sets a flag the voice thread reads; it never touches call control, so a
 * transient false on-hook just mutes for one poll and self-heals.
 */
void pcm_en751221_voice_set_mute(int on)
{
	WRITE_ONCE(voice_tx_mute, on ? 1 : 0);
}
EXPORT_SYMBOL_GPL(pcm_en751221_voice_set_mute);

int pcm_en751221_voice_start(void)
{
	struct pcm_dev *p = g_pcm;
	static const u32 oem_slots[4] = {
		0x10301020, 0x10501040, 0x10701060, 0x10901080,
	};
	int d, ch, ret;

	if (!p)
		return -ENODEV;
	if (vs.active)
		return -EBUSY;

	if (!vs.txb) {
		vs.txb = dmam_alloc_coherent(p->dev, PCM_DESC_NUM * VOICE_FB + 64,
					     &vs.txb_dma, GFP_KERNEL);
		vs.rxb = dmam_alloc_coherent(p->dev,
					     PCM_DESC_NUM * VOICE_CHANS * VOICE_FB + 64,
					     &vs.rxb_dma, GFP_KERNEL);
		if (!vs.txb || !vs.rxb)
			return -ENOMEM;
	}
	ret = kfifo_alloc(&vs.cap, voice_fifo_sz, GFP_KERNEL);
	if (ret)
		return ret;
	ret = kfifo_alloc(&vs.play, voice_fifo_sz, GFP_KERNEL);
	if (ret) {
		kfifo_free(&vs.cap);
		return ret;
	}
	init_waitqueue_head(&vs.cap_wq);
	init_waitqueue_head(&vs.play_wq);
	init_waitqueue_head(&vs.irq_wq);
	WRITE_ONCE(vs.irq_pending, 0);
	memset(vs.txb, 0, PCM_DESC_NUM * VOICE_FB);
	memset(vs.rxb, 0, PCM_DESC_NUM * VOICE_CHANS * VOICE_FB);

	for (d = 0; d < 4; d++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + d, oem_slots[d]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + d, oem_slots[d]);
	}
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~(PCM_DMA_TX_EN | PCM_DMA_RX_EN));
	pcm_wr(p, PCM_ISR, pcm_rd(p, PCM_ISR));

	for (d = 0; d < PCM_DESC_NUM; d++) {
		memset(&p->tx_ring[d], 0, sizeof(struct pcm_desc));
		memset(&p->rx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < VOICE_CHANS; ch++) {
			p->tx_ring[d].buf_addr[ch] =
				(u32)((vs.txb_dma + d * VOICE_FB) & PCM_DMA_ADDR_MASK);
			p->rx_ring[d].buf_addr[ch] =
				(u32)((vs.rxb_dma + (d * VOICE_CHANS + ch) * VOICE_FB) &
				      PCM_DMA_ADDR_MASK);
		}
		p->tx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, VOICE_TX_CH_VALID) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
		p->rx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, VOICE_RX_CH_VALID) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
	}

	pcm_wr(p, PCM_TX_DESC_RING_BASE, (u32)(p->tx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, 0xf5071306);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, VOICE_RX_CH_VALID) |
	       PCM_DMA_TX_EN | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);

	/* Acknowledge stale status on every new stream. Only unmask descriptor
	 * interrupts when the experimental IRQ service mode will consume them;
	 * polling needs no 200 Hz interrupt load. */
	pcm_wr(p, PCM_ISR, pcm_rd(p, PCM_ISR));
	pcm_wr(p, PCM_IMR, READ_ONCE(voice_use_irq) ?
	       PCM_INT_TX_DESC_UPDATE | PCM_INT_RX_DESC_UPDATE : 0);

	WRITE_ONCE(voice_tx_mute, 0);	/* a new call always starts un-muted */
	vs.thr = kthread_run(pcm_voice_thread, NULL, "pcm-voice");
	if (IS_ERR(vs.thr)) {
		kfifo_free(&vs.cap);
		kfifo_free(&vs.play);
		return PTR_ERR(vs.thr);
	}
	vs.active = true;
	return 0;
}
EXPORT_SYMBOL_GPL(pcm_en751221_voice_start);

void pcm_en751221_voice_stop(void)
{
	struct pcm_dev *p = g_pcm;

	if (!vs.active)
		return;
	vs.active = false;
	if (p)
		pcm_wr(p, PCM_IMR, 0);	/* mask before tearing down (no stray wakes) */
	if (vs.thr)
		kthread_stop(vs.thr);
	vs.thr = NULL;
	if (p)
		pcm_wr(p, PCM_TX_RX_DMA_CTRL,
		       pcm_rd(p, PCM_TX_RX_DMA_CTRL) &
		       ~(PCM_DMA_TX_EN | PCM_DMA_RX_EN));
	kfifo_free(&vs.cap);
	kfifo_free(&vs.play);
}
EXPORT_SYMBOL_GPL(pcm_en751221_voice_stop);

/* Blocking read of captured mic PCM (16-bit linear). */
ssize_t pcm_en751221_voice_read(char __user *ubuf, size_t len)
{
	unsigned int copied = 0;
	int ret;

	if (!vs.active)
		return -ENODEV;
	if (kfifo_is_empty(&vs.cap)) {
		ret = wait_event_interruptible(vs.cap_wq,
					       !kfifo_is_empty(&vs.cap) || !vs.active);
		if (ret)
			return ret;
		if (!vs.active)
			return 0;
	}
	ret = kfifo_to_user(&vs.cap, ubuf, len, &copied);
	return ret ? ret : copied;
}
EXPORT_SYMBOL_GPL(pcm_en751221_voice_read);

/* Blocking write of playback PCM (16-bit linear) to the earpiece. */
ssize_t pcm_en751221_voice_write(const char __user *ubuf, size_t len)
{
	unsigned int copied = 0;
	int ret;

	if (!vs.active)
		return -ENODEV;
	if (kfifo_is_full(&vs.play)) {
		ret = wait_event_interruptible(vs.play_wq,
					       !kfifo_is_full(&vs.play) || !vs.active);
		if (ret)
			return ret;
		if (!vs.active)
			return -ENODEV;
	}
	ret = kfifo_from_user(&vs.play, ubuf, len, &copied);
	return ret ? ret : copied;
}
EXPORT_SYMBOL_GPL(pcm_en751221_voice_write);

static s16 pcm_tone_1khz_sample(int n)
{
	switch (n & 7) {
	case 0:
	case 4:
		return 0;
	case 1:
	case 3:
		return 14142;
	case 2:
		return 20000;
	case 5:
	case 7:
		return -14142;
	default:
		return -20000;
	}
}

static void pcm_fill_tone_1khz_frame(u8 *dst, int sample_base)
{
	int i;

	for (i = 0; i < PCM_SAMP_PER_FRAME; i++) {
		s16 s = pcm_tone_1khz_sample(sample_base + i);

		dst[i * 2] = (u8)(s >> 8);
		dst[i * 2 + 1] = (u8)(s & 0xff);
	}
}

int pcm_en751221_play_tone_1khz(void)
{
	struct pcm_dev *p = g_pcm;
	static const u32 oem_slots[4] = {
		0x10301020, 0x10501040, 0x10701060, 0x10901080,
	};
	static void *txb, *rxb;
	static dma_addr_t txb_dma, rxb_dma;
	const int total_frames = 1000;	/* 10 seconds @ 100 frames/s */
	int frame = 0, d, ch, guard = 0;

	if (!p)
		return -ENODEV;
	if (!txb) {
		txb = dmam_alloc_coherent(p->dev,
					  PCM_DESC_NUM * PCM_BYTES_PER_FRAME + 64,
					  &txb_dma, GFP_KERNEL);
		rxb = dmam_alloc_coherent(p->dev, 8 * PCM_BYTES_PER_FRAME + 64,
					  &rxb_dma, GFP_KERNEL);
		if (!txb || !rxb)
			return -ENOMEM;
	}

	for (d = 0; d < 4; d++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + d, oem_slots[d]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + d, oem_slots[d]);
	}

	for (d = 0; d < PCM_DESC_NUM && frame < total_frames; d++, frame++) {
		memset(&p->tx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < 8; ch++)
			p->tx_ring[d].buf_addr[ch] =
				(u32)((txb_dma + d * PCM_BYTES_PER_FRAME) &
				      PCM_DMA_ADDR_MASK);
		pcm_fill_tone_1khz_frame((u8 *)txb + d * PCM_BYTES_PER_FRAME,
					 frame * PCM_SAMP_PER_FRAME);
		p->tx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
	}

	for (d = 0; d < PCM_DESC_NUM; d++) {
		memset(&p->rx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < 8; ch++)
			p->rx_ring[d].buf_addr[ch] =
				(u32)((rxb_dma + ch * PCM_BYTES_PER_FRAME) &
				      PCM_DMA_ADDR_MASK);
		p->rx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
	}

	pcm_wr(p, PCM_TX_DESC_RING_BASE, (u32)(p->tx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, 0xf5071306);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, 0xff) |
	       PCM_DMA_TX_EN | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);

	while (frame < total_frames && guard < 4000) {
		int progressed = 0;

		for (d = 0; d < PCM_DESC_NUM && frame < total_frames; d++) {
			dma_rmb();
			if (READ_ONCE(p->tx_ring[d].status) & PCM_DESC_OWN)
				continue;
			pcm_fill_tone_1khz_frame((u8 *)txb + d * PCM_BYTES_PER_FRAME,
						 frame * PCM_SAMP_PER_FRAME);
			frame++;
			dma_wmb();
			p->tx_ring[d].status = PCM_DESC_OWN |
				FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
				FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
			pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);
			progressed = 1;
		}
		for (d = 0; d < PCM_DESC_NUM; d++) {
			if (READ_ONCE(p->rx_ring[d].status) & PCM_DESC_OWN)
				continue;
			p->rx_ring[d].status = PCM_DESC_OWN |
				FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
				FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
		}
		pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);
		if (!progressed) {
			usleep_range(2000, 4000);
			guard++;
		}
	}
	msleep(220);
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~PCM_DMA_TX_EN);
	return (frame >= total_frames) ? 0 : -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(pcm_en751221_play_tone_1khz);

/*
 * Loopback test: transmit a known ramp (0,1,2,...79) on all 8 TX slots while
 * capturing RX channel 2 at the same time. With the SLIC's TSA loopback armed
 * by the caller, the bytes we send come straight back, so a ramp in out[]
 * proves the SoC TX -> bus -> SoC RX digital path (DMA + slot map) works end
 * to end -- independent of the analog earpiece. Returns ch2 (80 bytes).
 */
int pcm_en751221_loopback_capture(u8 *out, u32 intface)
{
	struct pcm_dev *p = g_pcm;
	static const u32 oem_slots[4] = {
		0x10301020, 0x10501040, 0x10701060, 0x10901080,
	};
	static void *txb, *rxb;
	static dma_addr_t txb_dma, rxb_dma;
	int i, d, ch, done = -1;

	if (!p)
		return -ENODEV;
	if (!txb) {
		txb = dmam_alloc_coherent(p->dev, PCM_BYTES_PER_FRAME + 64,
					  &txb_dma, GFP_KERNEL);
		rxb = dmam_alloc_coherent(p->dev,
					  PCM_DESC_NUM * 8 * PCM_BYTES_PER_FRAME + 64,
					  &rxb_dma, GFP_KERNEL);
		if (!txb || !rxb)
			return -ENOMEM;
	}
	for (i = 0; i < PCM_BYTES_PER_FRAME; i++)	/* ramp pattern */
		((u8 *)txb)[i] = (u8)i;
	memset(rxb, 0, PCM_DESC_NUM * 8 * PCM_BYTES_PER_FRAME);

	for (d = 0; d < 4; d++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + d, oem_slots[d]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + d, oem_slots[d]);
	}
	for (d = 0; d < PCM_DESC_NUM; d++) {
		memset(&p->tx_ring[d], 0, sizeof(struct pcm_desc));
		memset(&p->rx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < 8; ch++) {
			p->tx_ring[d].buf_addr[ch] =
				(u32)(txb_dma & PCM_DMA_ADDR_MASK);
			p->rx_ring[d].buf_addr[ch] =
				(u32)((rxb_dma + (d * 8 + ch) * PCM_BYTES_PER_FRAME) &
				      PCM_DMA_ADDR_MASK);
		}
		p->tx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
		p->rx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
	}

	pcm_wr(p, PCM_TX_DESC_RING_BASE, (u32)(p->tx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, intface);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, 0xff) |
	       PCM_DMA_TX_EN | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);

	/* let a few frames loop through, then grab a completed RX descriptor */
	for (i = 0; i < 500; i++) {
		dma_rmb();
		for (d = PCM_DESC_NUM - 1; d >= 0; d--) {
			if (!(READ_ONCE(p->rx_ring[d].status) & PCM_DESC_OWN)) {
				done = d;
				break;
			}
		}
		if (done >= 0 && i > 20)	/* give loopback pipeline time */
			break;
		usleep_range(1000, 2000);
	}
	dma_rmb();
	if (done < 0)
		done = 0;
	memcpy(out, (u8 *)rxb + (done * 8 + 2) * PCM_BYTES_PER_FRAME,
	       PCM_BYTES_PER_FRAME);				/* ch2 */
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~(PCM_DMA_TX_EN | PCM_DMA_RX_EN));
	return (done >= 0) ? 0 : -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(pcm_en751221_loopback_capture);

int pcm_en751221_loopback_tone_capture(u8 *out, int nbytes)
{
	struct pcm_dev *p = g_pcm;
	static const u32 oem_slots[4] = {
		0x10301020, 0x10501040, 0x10701060, 0x10901080,
	};
	static void *txb, *rxb;
	static dma_addr_t txb_dma, rxb_dma;
	int i, d, ch, done = -1;

	if (!p)
		return -ENODEV;
	if (nbytes > 8 * PCM_BYTES_PER_FRAME)
		nbytes = 8 * PCM_BYTES_PER_FRAME;
	if (!txb) {
		txb = dmam_alloc_coherent(p->dev,
					  PCM_DESC_NUM * PCM_BYTES_PER_FRAME + 64,
					  &txb_dma, GFP_KERNEL);
		rxb = dmam_alloc_coherent(p->dev,
					  PCM_DESC_NUM * 8 * PCM_BYTES_PER_FRAME + 64,
					  &rxb_dma, GFP_KERNEL);
		if (!txb || !rxb)
			return -ENOMEM;
	}
	memset(rxb, 0, PCM_DESC_NUM * 8 * PCM_BYTES_PER_FRAME);

	for (d = 0; d < 4; d++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + d, oem_slots[d]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + d, oem_slots[d]);
	}
	for (d = 0; d < PCM_DESC_NUM; d++) {
		memset(&p->tx_ring[d], 0, sizeof(struct pcm_desc));
		memset(&p->rx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < 8; ch++) {
			p->tx_ring[d].buf_addr[ch] =
				(u32)((txb_dma + d * PCM_BYTES_PER_FRAME) &
				      PCM_DMA_ADDR_MASK);
			p->rx_ring[d].buf_addr[ch] =
				(u32)((rxb_dma + (d * 8 + ch) * PCM_BYTES_PER_FRAME) &
				      PCM_DMA_ADDR_MASK);
		}
		pcm_fill_tone_1khz_frame((u8 *)txb + d * PCM_BYTES_PER_FRAME,
					 d * PCM_SAMP_PER_FRAME);
		p->tx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
		p->rx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
	}

	pcm_wr(p, PCM_TX_DESC_RING_BASE, (u32)(p->tx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, 0xf5071306);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, 0xff) |
	       PCM_DMA_TX_EN | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);

	for (i = 0; i < 500; i++) {
		dma_rmb();
		for (d = PCM_DESC_NUM - 1; d >= 0; d--) {
			if (!(READ_ONCE(p->rx_ring[d].status) & PCM_DESC_OWN)) {
				done = d;
				break;
			}
		}
		if (done >= 0 && i > 20)
			break;
		usleep_range(1000, 2000);
	}
	dma_rmb();
	if (done < 0)
		done = 0;
	memcpy(out, (u8 *)rxb + done * 8 * PCM_BYTES_PER_FRAME, nbytes);
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~(PCM_DMA_TX_EN | PCM_DMA_RX_EN));
	return (done >= 0) ? 0 : -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(pcm_en751221_loopback_tone_capture);

static irqreturn_t pcm_irq(int irq, void *data)
{
	struct pcm_dev *p = data;
	u32 isr = pcm_rd(p, PCM_ISR);

	if (!isr)
		return IRQ_NONE;

	pcm_wr(p, PCM_ISR, isr);		/* W1C acknowledge (clears the level) */

	/* A descriptor completed -> wake the voice thread so it drains/refills the
	 * rings. This replaces the thread's usleep busy-poll when voice_use_irq=1. */
	if (isr & (PCM_INT_TX_DESC_UPDATE | PCM_INT_RX_DESC_UPDATE)) {
		WRITE_ONCE(vs.irq_pending, 1);
		wake_up_interruptible(&vs.irq_wq);
	}
	/* Ring starvation / bus error: latched + logged, not fatal (the thread's
	 * re-arm + polling-demand recovers). 0x3d in the ISR is this case. */
	if (isr & (PCM_INT_TX_BUF_UNDER_RUN | PCM_INT_RX_BUF_OVER_RUN |
		   PCM_INT_AHB_BUS_ERR))
		dev_dbg(p->dev, "PCM IRQ underrun/overrun ISR=0x%08x\n", isr);
	return IRQ_HANDLED;
}

static int pcm_regs_show(struct seq_file *s, void *unused)
{
	struct pcm_dev *p = s->private;
	void __iomem *intc;
	int i;

	for (i = 0; i < PCM_REG_COUNT; i++)
		seq_printf(s, "reg[%2d] @0x%02x = 0x%08x\n",
			   i, PCM_REG(i), pcm_rd(p, i));

	/* Find the PCM's real intc input: dump the EN751221 intc (0x1fb40000).
	 * Read PENDING as-is, then briefly unmask everything (local IRQs off so the
	 * chained handler can't dispatch) to capture the RAW pending, then restore.
	 * Compare the dump during a call (PCM asserting) vs idle: the bit that turns
	 * on is the PCM's line. MASK0/1=0x04/0x50, PEND0/1=0x08/0x54. */
	intc = ioremap(0x1fb40000, 0x100);
	if (intc) {
		unsigned long flags;
		u32 racc0 = 0, racc1 = 0;
		int k;

		/* OEM intc reg map (tc3162.h): ITR(type)@00 IMR(mask)@04 IPR(pend)@08
		 * ISR(status)@0c IPRn(priority)@10-2c IVSR@30-4c IMR_1@50 IPR_1@54.
		 * The mainline driver only uses 04/08/50/54 -- dump the rest to find
		 * why the PCM line never reaches the CPU. Read raw (no mask change). */
		seq_printf(s, "intc ITR  @00 = 0x%08x (type edge/level)\n",
			   ioread32(intc + 0x00));
		seq_printf(s, "intc IMR0 @04 = 0x%08x   IMR1 @50 = 0x%08x (mask)\n",
			   ioread32(intc + 0x04), ioread32(intc + 0x50));
		seq_printf(s, "intc IPR0 @08 = 0x%08x   IPR1 @54 = 0x%08x (pending)\n",
			   ioread32(intc + 0x08), ioread32(intc + 0x54));
		seq_printf(s, "intc ISR  @0c = 0x%08x (raw status)\n",
			   ioread32(intc + 0x0c));
		for (i = 0x10; i <= 0x2c; i += 4)
			seq_printf(s, "intc IPRn @%02x = 0x%08x (priority)\n",
				   i, ioread32(intc + i));
		for (i = 0x30; i <= 0x4c; i += 4)
			seq_printf(s, "intc IVSR @%02x = 0x%08x (vpe route)\n",
				   i, ioread32(intc + i));
		seq_printf(s, "intc IPSR @58 = 0x%08x   @5c = 0x%08x\n",
			   ioread32(intc + 0x58), ioread32(intc + 0x5c));
		seq_printf(s, "intc IVSR @60 = 0x%08x   @64 = 0x%08x\n",
			   ioread32(intc + 0x60), ioread32(intc + 0x64));

		/* OR-sample IPR over ~10ms with all inputs unmasked (IRQs off per
		 * burst, so no dispatch) to catch a brief/edge pending the single read
		 * could miss. Catching bit30(timer, 1kHz) proves the sampler works. */
		for (k = 0; k < 2000; k++) {
			u32 sm0, sm1;

			local_irq_save(flags);
			sm0 = ioread32(intc + 0x04);
			sm1 = ioread32(intc + 0x50);
			iowrite32(~0u, intc + 0x04);
			iowrite32(~0u, intc + 0x50);
			racc0 |= ioread32(intc + 0x08);
			racc1 |= ioread32(intc + 0x54);
			iowrite32(sm0, intc + 0x04);
			iowrite32(sm1, intc + 0x50);
			local_irq_restore(flags);
			udelay(5);
		}
		seq_printf(s, "intc orIPR0=0x%08x  orIPR1=0x%08x (2000 samp ~10ms, all-unmasked)\n",
			   racc0, racc1);
		iounmap(intc);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pcm_regs);

/*
 * Self-contained PCM IRQ delivery test: arm the voice engine (voice_start ->
 * DMA TX/RX on, IMR=0x0c, descriptors clocking), OR-sample the intc raw status
 * for ~20ms to see if the PCM asserts ANY intc input, then disarm. No SIP call
 * needed. Compare against idle: any new bit in orISR(0c)/orIPR vs idle = the PCM
 * line. Returns -EBUSY if a real call is in progress (engine already active).
 */
static int pcm_irqtest_show(struct seq_file *s, void *unused)
{
	struct pcm_dev *p = s->private;
	void __iomem *intc;
	u32 isr, imr, dma, racc0 = 0, racc1 = 0, isracc = 0;
	unsigned long flags;
	int k, ret;

	ret = pcm_en751221_voice_start();
	if (ret) {
		seq_printf(s, "voice_start failed: %d (a call may be active)\n", ret);
		return 0;
	}
	msleep(30);				/* let the DMA clock + descriptors complete */

	isr = pcm_rd(p, PCM_ISR);
	imr = pcm_rd(p, PCM_IMR);
	dma = pcm_rd(p, PCM_TX_RX_DMA_CTRL);

	intc = ioremap(0x1fb40000, 0x100);
	if (intc) {
		for (k = 0; k < 4000; k++) {	/* ~20ms armed window */
			u32 sm0, sm1;

			local_irq_save(flags);
			sm0 = ioread32(intc + 0x04);
			sm1 = ioread32(intc + 0x50);
			iowrite32(~0u, intc + 0x04);
			iowrite32(~0u, intc + 0x50);
			racc0 |= ioread32(intc + 0x08);
			racc1 |= ioread32(intc + 0x54);
			isracc |= ioread32(intc + 0x0c);
			iowrite32(sm0, intc + 0x04);
			iowrite32(sm1, intc + 0x50);
			local_irq_restore(flags);
			udelay(5);
		}
		iounmap(intc);
	}

	pcm_en751221_voice_stop();

	seq_printf(s, "armed: PCM ISR=0x%08x IMR=0x%08x DMA=0x%08x  (want ISR&IMR!=0, DMA=0x0f000003)\n",
		   isr, imr, dma);
	seq_printf(s, "intc over ~20ms armed: orIPR0=0x%08x orIPR1=0x%08x orISR(0c)=0x%08x\n",
		   racc0, racc1, isracc);
	seq_puts(s, "(idle baseline orIPR0=0x60800008 = bits 3,23,29,30 timer+nbrs; a NEW bit 11/12/13 = PCM)\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pcm_irqtest);

static int pcm_loopback_selftest_show(struct seq_file *s, void *unused)
{
	struct pcm_dev *p = s->private;
	int ret = pcm_loopback_selftest(p);

	if (ret)
		seq_printf(s, "BACK_TO_BACK loopback: failed (%d)\n", ret);
	else
		seq_puts(s, "BACK_TO_BACK loopback: ok\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pcm_loopback_selftest);

static int pcm_rx_scan2_show(struct seq_file *s, void *unused)
{
	struct pcm_dev *p = s->private;
	static const u32 oem_slots[4] = {
		0x10301020, 0x10501040, 0x10701060, 0x10901080,
	};
	static void *rxb;
	static dma_addr_t rxb_dma;
	const int chans = 8;
	int d, ch, i, done = -1, ret = 0;

	if (!rxb) {
		rxb = dmam_alloc_coherent(p->dev,
					  PCM_DESC_NUM * chans * PCM_BYTES_PER_FRAME + 64,
					  &rxb_dma, GFP_KERNEL);
		if (!rxb)
			return -ENOMEM;
	}

	mutex_lock(&p->selftest_lock);
	memset(rxb, 0, PCM_DESC_NUM * chans * PCM_BYTES_PER_FRAME);

	for (i = 0; i < 4; i++)
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + i, oem_slots[i]);

	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~PCM_DMA_RX_EN);
	pcm_wr(p, PCM_ISR, pcm_rd(p, PCM_ISR));

	for (d = 0; d < PCM_DESC_NUM; d++) {
		memset(&p->rx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < chans; ch++)
			p->rx_ring[d].buf_addr[ch] =
				(u32)((rxb_dma + (d * chans + ch) * PCM_BYTES_PER_FRAME) &
				      PCM_DMA_ADDR_MASK);
		p->rx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
	}

	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, 0xf5071306);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, 0xff) | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);

	for (i = 0; i < 500; i++) {
		dma_rmb();
		for (d = 0; d < PCM_DESC_NUM; d++) {
			if (!(READ_ONCE(p->rx_ring[d].status) & PCM_DESC_OWN)) {
				done = d;
				break;
			}
		}
		if (done >= 0)
			break;
		usleep_range(1000, 2000);
	}
	dma_rmb();
	if (done < 0) {
		done = 0;
		ret = -ETIMEDOUT;
	}

	seq_printf(s, "rx_scan2 ret=%d desc=%d stride=%u chValid=0xff\n",
		   ret, done, PCM_BYTES_PER_FRAME);
	for (ch = 0; ch < chans; ch++) {
		u8 *b = (u8 *)rxb + (done * chans + ch) * PCM_BYTES_PER_FRAME;
		s16 mn = 32767, mx = -32768;
		int peak = 0;

		for (i = 0; i < PCM_SAMP_PER_FRAME; i++) {
			s16 v = (s16)((b[i * 2] << 8) | b[i * 2 + 1]);
			int mag = (v == -32768) ? 32768 : abs(v);

			if (v < mn)
				mn = v;
			if (v > mx)
				mx = v;
			if (mag > peak)
				peak = mag;
		}
		seq_printf(s, " ch%d off=%3u min=%6d max=%6d pp=%6d peak=%5d %s\n",
			   ch, ch * PCM_BYTES_PER_FRAME, mn, mx, mx - mn, peak,
			   (mx - mn > 256 || peak > 256) ? "<== SIGNAL" : "");
	}

	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~PCM_DMA_RX_EN);
	mutex_unlock(&p->selftest_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pcm_rx_scan2);

static int pcm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pcm_dev *p;
	u32 chip;
	int ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;

	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);

	p->scu = devm_ioremap(dev, SCU_PHYS_BASE, 0x970);
	if (!p->scu)
		return -ENOMEM;

	p->chip_scu = devm_ioremap(dev, CHIP_SCU_PHYS_BASE, 0x388);
	if (!p->chip_scu)
		return -ENOMEM;

	mutex_init(&p->selftest_lock);

	chip = readl(p->scu + SCU_CHIP_ID) & SCU_CHIP_ID_MASK;
	if (chip != SCU_CHIP_EN751221 && chip != SCU_CHIP_EN7526C)
		dev_warn(dev, "unexpected chip id 0x%08x (continuing)\n", chip);
	else
		dev_info(dev, "EN751221-class SoC (chip id 0x%08x)\n", chip);

	pcm_soft_reset(p);
	pcm_load_defaults(p);

	ret = pcm_alloc_rings(p);
	if (ret)
		return ret;

	/* IRQ is optional: a wrong DT number must not block bring-up.
	 * pcm_hwirq>=0 overrides the DT line (hot-test the real PCM intc input). */
	if (pcm_hwirq >= 0) {
		struct device_node *in = of_find_compatible_node(NULL, NULL,
							"econet,en751221-intc");
		struct irq_domain *dom = in ? irq_find_host(in) : NULL;

		of_node_put(in);
		p->irq = dom ? irq_create_mapping(dom, pcm_hwirq) : 0;
		dev_info(dev, "pcm_hwirq override: hwirq %d -> virq %d\n",
			 pcm_hwirq, p->irq);
	} else {
		p->irq = platform_get_irq_optional(pdev, 0);
	}
	if (p->irq > 0) {
		ret = devm_request_irq(dev, p->irq, pcm_irq, 0,
				       dev_name(dev), p);
		if (ret)
			dev_warn(dev, "could not request IRQ %d: %d\n",
				 p->irq, ret);
	}

	p->dbg = debugfs_create_dir("pcm-en751221", NULL);
	debugfs_create_file("regs", 0444, p->dbg, p, &pcm_regs_fops);
	debugfs_create_file("irqtest", 0444, p->dbg, p, &pcm_irqtest_fops);
	debugfs_create_file("loopback_selftest", 0444, p->dbg, p,
			    &pcm_loopback_selftest_fops);
	debugfs_create_file("rx_scan2", 0444, p->dbg, p, &pcm_rx_scan2_fops);

	platform_set_drvdata(pdev, p);
	g_pcm = p;	/* for pcm_en751221_zsi_clock_run() (SLIC ZSI clock) */
	dev_info(dev, "PCM/TDM controller ready (phase 1: reset + defaults)\n");
	return 0;
}

static void pcm_remove(struct platform_device *pdev)
{
	struct pcm_dev *p = platform_get_drvdata(pdev);

	debugfs_remove_recursive(p->dbg);
	pcm_wr(p, PCM_IMR, 0);
}

static const struct of_device_id pcm_of_match[] = {
	{ .compatible = "econet,en751221-pcm" },
	{ }
};
MODULE_DEVICE_TABLE(of, pcm_of_match);

static struct platform_driver pcm_driver = {
	.probe = pcm_probe,
	.remove = pcm_remove,
	.driver = {
		.name = "pcm-en751221",
		.of_match_table = pcm_of_match,
	},
};
module_platform_driver(pcm_driver);

MODULE_DESCRIPTION("EcoNet/TrendChip EN751221 PCM/TDM controller");
MODULE_LICENSE("GPL");

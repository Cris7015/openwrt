/* SPDX-License-Identifier: GPL-2.0-only */
#include "pcm-dual.h"
/* Two FXS streams share one continuously running PCM ring. The engine is
 * started before calls, and never reset when a second line opens or closes.
 * Slots 4/6 have separate DMA slabs and separate userspace queues. */
struct pcm_dual_line {
	struct kfifo cap, play;
	spinlock_t lock;
	wait_queue_head_t cap_wq, play_wq;
	bool opened;
	bool mute_cap;		/* on-hook seen while open: send silence (DC-DC buzz) */
	u32 read_bytes, write_bytes;
	u32 tx_short_frames, cap_drop_frames;
};

static struct {
	struct pcm_dual_line line[2];
	void *tx, *rx;
	dma_addr_t tx_dma, rx_dma;
	struct task_struct *thread;
	struct dentry *dbg;
	u32 rx_frames, tx_frames;
	s16 rx_min[4], rx_max[4];
	/* IRQ-driven service (dual_use_irq): pcm_irq sets irq_pending and wakes
	 * the thread on TX/RX descriptor completion; counters for dual_status. */
	wait_queue_head_t irq_wq;
	int irq_pending;
	u32 irq_wakes, timeout_wakes, isr_events;
} pd;

/* Hardware mapping is independently tunable for validation of both buses. */
/* With the current four-channel engine, live answered-call telemetry finds
 * signal in DMA0/1; DMA2/3 remain untouched. The old eight-channel scan's
 * slot6 -> DMA2 mapping does not apply to this configuration. */
static unsigned int dual_rx_ch[2] = { 0, 1 };
/* TX0 is audible on Phone1. TX1 for Phone2 is the next controlled test;
 * do not treat RX symmetry alone as proof of the transmit mapping. */
static unsigned int dual_tx_ch[2] = { 0, 1 };
module_param_array(dual_rx_ch, uint, NULL, 0444);
module_param_array(dual_tx_ch, uint, NULL, 0444);

/* Bound queued playback without changing the shared DMA ring or RX capacity.
 * 640 bytes = 40 ms at 8 kHz s16 mono; use 4096 for the previous baseline. */
static unsigned int dual_play_limit = 640;
module_param(dual_play_limit, uint, 0444);

/* Per-channel min/max telemetry for dual_status costs 32k u-law decodes per
 * second on all four DMA channels, 24/7. Keep it off unless diagnosing. */
static bool dual_telemetry;
module_param(dual_telemetry, bool, 0644);

/* Service the rings from the PCM descriptor-completion interrupt instead of
 * the 0.5-1 ms usleep poll: one wake per 10 ms frame, deterministic cadence,
 * and the downstream jitter/audio buffers can shrink. The timeout is only a
 * safety net so a missed interrupt never wedges audio (polling at that rate). */
static int dual_use_irq = 1;
module_param(dual_use_irq, int, 0644);
/* 20 ms: two frames. 5 ms fired ~45 times/s because TX and RX completions
 * alternate ~5 ms apart; 20 ms never fired in the 19-sep soak. */
static int dual_irq_timeout_ms = 20;
module_param(dual_irq_timeout_ms, int, 0644);

/* Read-only telemetry: no register writes, audio recording or engine reset. */
static int pcm_dual_status_show(struct seq_file *s, void *unused)
{
	int i;
	unsigned long flags;

	seq_printf(s, "running=%u rx_frames=%u tx_frames=%u play_limit=%u\n",
		READ_ONCE(pcm_dual_running), READ_ONCE(pd.rx_frames), READ_ONCE(pd.tx_frames), dual_play_limit);
	seq_printf(s, "irq_mode=%d irq_wakes=%u timeout_wakes=%u isr_events=%u\n",
		READ_ONCE(dual_use_irq), READ_ONCE(pd.irq_wakes),
		READ_ONCE(pd.timeout_wakes), READ_ONCE(pd.isr_events));
	for (i = 0; i < 4; i++)
		seq_printf(s, "rx_dma%d last_min=%d last_max=%d\n", i,
			READ_ONCE(pd.rx_min[i]), READ_ONCE(pd.rx_max[i]));
	for (i = 0; i < 2; i++) {
		struct pcm_dual_line *l = &pd.line[i];
		u32 cap, play, rd, wr, shorts, drops;
		bool opened;

		spin_lock_irqsave(&l->lock, flags);
		cap = kfifo_len(&l->cap); play = kfifo_len(&l->play);
		rd = l->read_bytes; wr = l->write_bytes; opened = l->opened;
		shorts = l->tx_short_frames; drops = l->cap_drop_frames;
		spin_unlock_irqrestore(&l->lock, flags);
		seq_printf(s, "phone%d opened=%u rx_dma=%u tx_dma=%u cap=%u play=%u read_bytes=%u write_bytes=%u tx_short_frames=%u cap_drop_frames=%u muted=%u\n",
			i + 1, opened, dual_rx_ch[i], dual_tx_ch[i], cap, play, rd, wr, shorts, drops, READ_ONCE(l->mute_cap));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pcm_dual_status);

static int pcm_dual_thread(void *unused)
{
	struct pcm_dev *p = g_pcm;
	int rx = 0, tx = 0, line, i, ch;
	s16 frame[PCM_SAMP_PER_FRAME];
	unsigned long flags;
	bool opened;

	while (!kthread_should_stop()) {
		while (!(READ_ONCE(p->rx_ring[rx].status) & PCM_DESC_OWN)) {
			dma_rmb();
			if (READ_ONCE(dual_telemetry)) {
				for (ch = 0; ch < 4; ch++) {
					s16 *raw = (s16 *)pd.rx + (rx * VOICE_CHANS + ch) * PCM_SAMP_PER_FRAME;
					s16 lo = 32767, hi = -32768;

					for (i = 0; i < PCM_SAMP_PER_FRAME; i++) {
						s16 sample = ulaw_decode(raw[i] & 0xff);
						lo = min(lo, sample); hi = max(hi, sample);
					}
					WRITE_ONCE(pd.rx_min[ch], lo); WRITE_ONCE(pd.rx_max[ch], hi);
				}
			}
			pd.rx_frames++;
			for (line = 0; line < 2; line++) {
				struct pcm_dual_line *l = &pd.line[line];
				s16 *src = (s16 *)pd.rx +
					(rx * VOICE_CHANS + dual_rx_ch[line]) * PCM_SAMP_PER_FRAME;

				if (!READ_ONCE(l->opened))
					continue;	/* nobody captures: skip the decode */
				if (READ_ONCE(l->mute_cap))
					memset(frame, 0, sizeof(frame));
				else
					for (i = 0; i < PCM_SAMP_PER_FRAME; i++)
						frame[i] = ulaw_decode(src[i] & 0xff);
				spin_lock_irqsave(&l->lock, flags);
				if (l->opened && kfifo_avail(&l->cap) >= VOICE_FB)
					kfifo_in(&l->cap, frame, VOICE_FB);
				else if (l->opened)
					l->cap_drop_frames++;
				spin_unlock_irqrestore(&l->lock, flags);
				wake_up_interruptible(&l->cap_wq);
			}
			dma_wmb();
			WRITE_ONCE(p->rx_ring[rx].status, PCM_DESC_OWN |
				FIELD_PREP(PCM_DESC_CH_VALID, VOICE_RX_CH_VALID) |
				FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME));
			rx = (rx + 1) % PCM_DESC_NUM;
		}
		while (!(READ_ONCE(p->tx_ring[tx].status) & PCM_DESC_OWN)) {
			s16 *base = (s16 *)pd.tx + tx * VOICE_CHANS * PCM_SAMP_PER_FRAME;
			pd.tx_frames++;

			for (ch = 0; ch < VOICE_CHANS; ch++)
				for (i = 0; i < PCM_SAMP_PER_FRAME; i++)
					base[ch * PCM_SAMP_PER_FRAME + i] = tx_msb ? 0xff00 : 0xff;
			for (line = 0; line < 2; line++) {
				struct pcm_dual_line *l = &pd.line[line];
				unsigned int n = 0;

				memset(frame, 0, sizeof(frame));
				spin_lock_irqsave(&l->lock, flags);
				if (l->opened) {
					n = kfifo_out(&l->play, frame, VOICE_FB);
					if (n < VOICE_FB)
						l->tx_short_frames++;
				}
				opened = l->opened;
				spin_unlock_irqrestore(&l->lock, flags);
				(void)n;
				if (!opened)
					continue;	/* the ring already holds u-law silence */
				for (i = 0; i < PCM_SAMP_PER_FRAME; i++) {
					u8 code = ulaw_encode(frame[i]);

					base[dual_tx_ch[line] * PCM_SAMP_PER_FRAME + i] =
						tx_msb ? (u16)code << 8 : code;
				}
				wake_up_interruptible(&l->play_wq);
			}
			dma_wmb();
			WRITE_ONCE(p->tx_ring[tx].status, PCM_DESC_OWN |
				FIELD_PREP(PCM_DESC_CH_VALID, VOICE_TX_CH_VALID) |
				FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME));
			tx = (tx + 1) % PCM_DESC_NUM;
		}
		pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);
		pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);
		if (READ_ONCE(dual_use_irq)) {
			int t = READ_ONCE(dual_irq_timeout_ms);
			long left;

			left = wait_event_interruptible_timeout(pd.irq_wq,
				READ_ONCE(pd.irq_pending) || kthread_should_stop(),
				msecs_to_jiffies(t > 0 ? t : 5));
			if (READ_ONCE(pd.irq_pending)) {
				WRITE_ONCE(pd.irq_pending, 0);
				pd.irq_wakes++;
			} else if (left == 0) {
				pd.timeout_wakes++;
			}
		} else {
			usleep_range(500, 1000);
		}
	}
	return 0;
}

int pcm_en751221_dual_start(void)
{
	struct pcm_dev *p = g_pcm;
	const u32 slots[] = { 0x10301020, 0x10501040, 0x10701060, 0x10901080 };
	size_t size = PCM_DESC_NUM * VOICE_CHANS * VOICE_FB;
	int line, d, ch, ret;

	if (!p)
		return -ENODEV;
	if (vs.active || pcm_dual_running)
		return -EBUSY;
	if (dual_play_limit < 2 * VOICE_FB || dual_play_limit > VOICE_FIFO_SZ ||
	    (dual_play_limit & 1))
		return -EINVAL;
	for (line = 0; line < 2; line++)
		if (dual_rx_ch[line] >= 4 || dual_tx_ch[line] >= 4)
			return -EINVAL;
	if (dual_rx_ch[0] == dual_rx_ch[1] || dual_tx_ch[0] == dual_tx_ch[1])
		return -EINVAL;
	if (!pd.tx)
		pd.tx = dmam_alloc_coherent(p->dev, size, &pd.tx_dma, GFP_KERNEL);
	if (!pd.rx)
		pd.rx = dmam_alloc_coherent(p->dev, size, &pd.rx_dma, GFP_KERNEL);
	if (!pd.tx || !pd.rx)
		return -ENOMEM;
	for (line = 0; line < 2; line++) {
		struct pcm_dual_line *l = &pd.line[line];

		spin_lock_init(&l->lock);
		init_waitqueue_head(&l->cap_wq);
		init_waitqueue_head(&l->play_wq);
		l->opened = false;
		l->read_bytes = l->write_bytes = 0;
		ret = kfifo_alloc(&l->cap, VOICE_FIFO_SZ, GFP_KERNEL);
		if (ret)
			goto free_queues;
		ret = kfifo_alloc(&l->play, VOICE_FIFO_SZ, GFP_KERNEL);
		if (ret)
			goto free_queues;
	}
	WRITE_ONCE(pd.irq_pending, 0);	/* irq_wq itself is initialised in probe */
	pd.irq_wakes = pd.timeout_wakes = pd.isr_events = 0;
	memset(pd.rx, 0, size);
	for (d = 0; d < size / sizeof(u16); d++)
		((u16 *)pd.tx)[d] = tx_msb ? 0xff00 : 0xff;
	pcm_wr(p, PCM_IMR, 0);
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
		pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~(PCM_DMA_TX_EN | PCM_DMA_RX_EN));
	for (d = 0; d < 4; d++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + d, slots[d]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + d, slots[d]);
	}
	for (d = 0; d < PCM_DESC_NUM; d++) {
		memset(&p->tx_ring[d], 0, sizeof(struct pcm_desc));
		memset(&p->rx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < VOICE_CHANS; ch++) {
			unsigned int offset = (d * VOICE_CHANS + ch) * VOICE_FB;

			p->tx_ring[d].buf_addr[ch] = (pd.tx_dma + offset) & PCM_DMA_ADDR_MASK;
			p->rx_ring[d].buf_addr[ch] = (pd.rx_dma + offset) & PCM_DMA_ADDR_MASK;
		}
		p->tx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, VOICE_TX_CH_VALID) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
		p->rx_ring[d].status = p->tx_ring[d].status;
	}
	pcm_wr(p, PCM_TX_DESC_RING_BASE, p->tx_dma & PCM_DMA_ADDR_MASK);
	pcm_wr(p, PCM_RX_DESC_RING_BASE, p->rx_dma & PCM_DMA_ADDR_MASK);
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, 0xf5071306);
	pcm_wr(p, PCM_ISR, pcm_rd(p, PCM_ISR));
	/* Unmask descriptor completion only when the thread will wait for it. */
	pcm_wr(p, PCM_IMR, READ_ONCE(dual_use_irq) ?
	       PCM_INT_TX_DESC_UPDATE | PCM_INT_RX_DESC_UPDATE : 0);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
		FIELD_PREP(PCM_DMA_CH_VALID_MASK, VOICE_RX_CH_VALID) |
		PCM_DMA_TX_EN | PCM_DMA_RX_EN);
	WRITE_ONCE(pcm_dual_running, true);
	pd.thread = kthread_run(pcm_dual_thread, NULL, "pcm-dual");
	if (!IS_ERR(pd.thread)) {
		pd.dbg = debugfs_create_file("dual_status", 0400, p->dbg, NULL, &pcm_dual_status_fops);
		return 0;
	}
	ret = PTR_ERR(pd.thread);
	pd.thread = NULL;
	WRITE_ONCE(pcm_dual_running, false);
	pcm_wr(p, PCM_IMR, 0);
	pcm_wr(p, PCM_TX_RX_DMA_CTRL, 0);
free_queues:
	for (line = 0; line < 2; line++) {
		kfifo_free(&pd.line[line].cap);
		kfifo_free(&pd.line[line].play);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(pcm_en751221_dual_start);

void pcm_en751221_dual_stop(void)
{
	int i;

	if (!pcm_dual_running)
		return;
	debugfs_remove(pd.dbg);
	pd.dbg = NULL;
	WRITE_ONCE(pcm_dual_running, false);
	pcm_wr(g_pcm, PCM_IMR, 0);	/* no wakes while the thread is torn down */
	kthread_stop(pd.thread);
	pd.thread = NULL;
	pcm_wr(g_pcm, PCM_TX_RX_DMA_CTRL,
		pcm_rd(g_pcm, PCM_TX_RX_DMA_CTRL) & ~(PCM_DMA_TX_EN | PCM_DMA_RX_EN));
	for (i = 0; i < 2; i++) {
		wake_up_interruptible(&pd.line[i].cap_wq);
		wake_up_interruptible(&pd.line[i].play_wq);
		kfifo_free(&pd.line[i].cap);
		kfifo_free(&pd.line[i].play);
	}
}
EXPORT_SYMBOL_GPL(pcm_en751221_dual_stop);

int pcm_en751221_dual_open(unsigned int index)
{
	struct pcm_dual_line *l;
	unsigned long flags;
	int ret = 0;

	if (index >= 2 || !pcm_dual_running)
		return -ENODEV;
	l = &pd.line[index];
	spin_lock_irqsave(&l->lock, flags);
	if (l->opened) {
		ret = -EBUSY;
	} else {
		kfifo_reset(&l->cap);
		kfifo_reset(&l->play);
		l->tx_short_frames = l->cap_drop_frames = 0;
		l->opened = true;
	l->mute_cap = false;
	}
	spin_unlock_irqrestore(&l->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(pcm_en751221_dual_open);

void pcm_en751221_dual_close(unsigned int index)
{
	struct pcm_dual_line *l;
	unsigned long flags;

	if (index >= 2)
		return;
	l = &pd.line[index];
	spin_lock_irqsave(&l->lock, flags);
	l->opened = false;
	l->mute_cap = false;
	kfifo_reset(&l->cap);
	kfifo_reset(&l->play);
	spin_unlock_irqrestore(&l->lock, flags);
	wake_up_interruptible(&l->cap_wq);
	wake_up_interruptible(&l->play_wq);
}
EXPORT_SYMBOL_GPL(pcm_en751221_dual_close);

/* The SLIC side saw the loop open (or close again) on a line with an open
 * stream. With the loop open in active feed the Le9642 buck-boost runs in
 * light-load bursts and the ADC picks up a ~600 Hz buzz; the far end heard it
 * for the manager's whole hook debounce (~0.5 s) before the BYE. */
void pcm_en751221_dual_mute(unsigned int index, bool mute)
{
	if (index < 2)
		WRITE_ONCE(pd.line[index].mute_cap, mute);
}
EXPORT_SYMBOL_GPL(pcm_en751221_dual_mute);

ssize_t pcm_en751221_dual_io(unsigned int index, char __user *buf, size_t len, bool capture)
{
	struct pcm_dual_line *l;
	s16 tmp[PCM_SAMP_PER_FRAME];
	unsigned long flags;
	unsigned int n;
	int ret;
	wait_queue_head_t *wq;

	if (index >= 2)
		return -EINVAL;
	l = &pd.line[index];
	wq = capture ? &l->cap_wq : &l->play_wq;

	if (!len)
		return 0;
	len = min_t(size_t, len, sizeof(tmp)) & ~1UL;
	if (!len)
		return -EINVAL;
	if (!capture && copy_from_user(tmp, buf, len))
		return -EFAULT;
	ret = wait_event_interruptible(*wq,
		!READ_ONCE(pcm_dual_running) || !READ_ONCE(l->opened) ||
		(capture ? kfifo_len(&l->cap) >= 2 :
		 kfifo_len(&l->play) + len <= dual_play_limit));
	if (ret)
		return ret;
	if (!pcm_dual_running || !l->opened)
		return -ENODEV;
	spin_lock_irqsave(&l->lock, flags);
	/* Recheck under the lock in case another writer used the available space. */
	if (!capture)
		len = min_t(size_t, len, dual_play_limit - min_t(unsigned int,
						kfifo_len(&l->play), dual_play_limit));
	n = capture ? kfifo_out(&l->cap, tmp, len) : kfifo_in(&l->play, tmp, len);
	if (capture)
		l->read_bytes += n;
	else
		l->write_bytes += n;
	spin_unlock_irqrestore(&l->lock, flags);
	if (capture && copy_to_user(buf, tmp, n))
		return -EFAULT;
	return n;
}
EXPORT_SYMBOL_GPL(pcm_en751221_dual_io);

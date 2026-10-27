/* SPDX-License-Identifier: GPL-2.0-only */
#include "pcm-dual.h"
/* Opt-in dual FXS: shared reset/setup once; subsequent controls are per line. */
static bool dual_fxs;
module_param(dual_fxs, bool, 0444);

/*
 * r16 EXPERIMENT -- soft standby->active transition on off-hook.
 *
 * Current dual behaviour (dual_soft_offhook = 0, unchanged default): the per
 * line `lpm` node ONLY writes STATE (0x0c standby / 0x23 active). It never
 * touches SWCTRL (0xe6): the buck-boost switcher is programmed once to HP
 * (0x6f) by slic_audio_setup() at slic_dual_init() and stays there forever,
 * because the switcher is COMMON to both channels of the Le9642 and dropping
 * it would starve the other jack's rail mid-call. (The legacy mono lpm_write()
 * in econet-slic.c does toggle it: standby -> STATE 0x0c then SWCTRL 0x65 LP;
 * active -> SWCTRL 0x6f HP first, then STATE 0x23.)
 *
 * With dual_soft_offhook = 1 the standby->active edge becomes two steps:
 *   1. SWCTRL = LP (0x65)          -- rail low, so the feed comes up softly
 *   2. STATE  = 0x23 (active+codec)
 *   3. wait ~20 ms                 -- let the loop current settle
 *   4. SWCTRL = HP (0x6f)          -- full rail for the call
 * The aim is to remove the audible "clack" the handset makes when the line
 * snaps from standby to a fully-powered active feed.
 *
 * SAFETY: because the switcher is shared, step 1 would dip the rail of a call
 * in progress on the other jack. The soft path is therefore SKIPPED whenever
 * the other line is not parked in standby; that line then gets the plain
 * single-write transition, i.e. exactly today's behaviour.
 */
static bool dual_soft_offhook;
module_param(dual_soft_offhook, bool, 0644);
MODULE_PARM_DESC(dual_soft_offhook,
		 "two-step standby->active on off-hook (LP, active, 20ms, HP); skipped while the other jack is powered");

/*
 * r17 -- ring trip.
 *
 * The Le9642 keeps ringing into a closed loop until software changes the
 * system state: the vendor VP-API ends the ring from its hook event handler
 * (Vp886ProcessHookEvent -> ringControl.ringTripExitSt) and masks the hook for
 * 100 ms AFTER that exit (VP886_HOOKFREEZE_RING_TRIP); it never masks it while
 * the bell is on. r15/r16 froze this line's hook sample for the whole ON burst
 * and let the manager look only in the OFF gap, so a lift during the burst
 * left the bell energized on a closed loop for up to a second. The older DECT
 * base on phone2 gives the line back in that case and the call is never
 * answered (r16 shared-ring test, 2026-09-18).
 *
 * With ring_trip (default on) a `hook` read during this line's ON burst reads
 * the SIGREG hook bit; the ring_trip_reads-th consecutive set bit ends the
 * ring on this channel at once (STATE 0x23, the same write as ring=0) and the
 * node reports 1 from then on. The line is latched "tripped": a ring=1 write
 * is ignored until the manager writes ring=0 (it does when it answers, and
 * when the call goes away) or until the loop opens again, so a false trip
 * costs at most one ON burst on this jack. Until it trips, the sample stays
 * frozen at the pre-ring value exactly as in r16.
 */
static bool ring_trip = true;
module_param(ring_trip, bool, 0644);
MODULE_PARM_DESC(ring_trip, "end a channel's ring as soon as its loop closes (VP-API behaviour)");
/* r18: 2 samples. With 1, the first read after the ON edge tripped phone2 on
 * a <100 ms SIGREG glitch (loop current inrush into the old DECT base's ringer
 * as 0x03 -> 0x07 starts the waveform): the raw bit was 0 again 100 ms later
 * (hookprobe 2026-09-18 06:42:50, t=448.5). Two consecutive samples >= 150 ms
 * apart ride through it; a real lift still trips within ~300 ms. */
static unsigned int ring_trip_reads = 2;
module_param(ring_trip_reads, uint, 0644);
MODULE_PARM_DESC(ring_trip_reads, "consecutive off-hook samples during the ON burst before the ring trips");

/*
 * In-call capture mute poller (hang-up "piii").
 *
 * When the handset goes down mid-call the far end hears the loop-open click
 * and then the DC-DC light-load buzz until the capture mute takes effect.
 * Without this poller the mute only follows the call manager's `hook` reads:
 * one slow ZSI read (EC + SIGREG, ~10-16 ms with sd.lock held) per 40 ms tick
 * of a userspace loop, so it lands ~90-140 ms after the click (25-oct RTP).
 *
 * With dual_mute_poll_ms > 0 a work item samples SIGREG every
 * dual_mute_poll_ms while at least one stream is open (never at idle): ONE
 * device-level read serves both jacks (bytes 0/1), no EC prefix, settle
 * zsi_fast_gap_us. A hook EDGE on an open, non-ringing line is confirmed by
 * a second read before the mute flips, so one garbled read costs nothing.
 * Muting is immediate; unmuting waits until the loop has stayed closed for
 * dual_unmute_ms, because the hook switch bounces at hang-up (two clicks
 * 30-40 ms apart in 3 of the 4 calls of 25-oct) and a re-close in between
 * must not let the second click through. While enabled the poller is the only
 * writer of the mute (the hook node no longer toggles it), so the two cannot
 * fight. It only toggles the capture mute: call control still waits for the
 * manager's 450 ms of stable on-hook, the ring trip and the lpm node are not
 * touched, and a ringing line is skipped. The next sample is never sooner
 * than 3x the last read's duration, which caps the poller's share of the ZSI
 * bus at ~25-33 % even if the fast gap is off or the bus is slow.
 *
 * dual_hook_from_poll: the `hook` node of an open, non-ringing line answers
 * from the poller's sample while it is fresh (< 2 periods + 5 ms) instead of
 * doing its own slow read. Same bit, fresher; the managers' 40 ms reads stop
 * competing for the bus during calls.
 */
static unsigned int dual_mute_poll_ms;
module_param(dual_mute_poll_ms, uint, 0644);
MODULE_PARM_DESC(dual_mute_poll_ms, "in-call SIGREG poll period for the capture mute (ms); 0 = off, manager-driven only");
static bool dual_hook_from_poll = true;
module_param(dual_hook_from_poll, bool, 0644);
MODULE_PARM_DESC(dual_hook_from_poll, "an open line's hook node reuses the mute poller's fresh sample");
static unsigned int dual_mute_max_errors = 5;
module_param(dual_mute_max_errors, uint, 0644);
MODULE_PARM_DESC(dual_mute_max_errors, "consecutive poller read errors before zsi_fast_gap_us falls back to 0");
static unsigned int dual_unmute_ms = 40;
module_param(dual_unmute_ms, uint, 0644);
MODULE_PARM_DESC(dual_unmute_ms, "loop closed this long before the poller lifts the mute (hang-up contact bounce, hook flash end)");

static struct delayed_work dual_mute_work;
static struct {			/* all under sd.lock */
	u32 reads, errors, busy, confirm_fail, fast_off, consecutive;
	s64 last_us, max_us;
} dmp;

struct slic_dual_line {
	struct miscdevice misc;
	unsigned int index;
	u8 ec, slot;
	bool ringing;
	bool powered;		/* last STATE written was not 0x0c standby */
	bool tripped;		/* ring ended by the loop closing; ring=1 ignored until ring=0 */
	unsigned int trip_cnt;	/* consecutive off-hook samples seen during the ON burst */
	int hook_cache;
	bool opened;		/* the PCM stream of this line is open (baresip) */
	bool muted;		/* capture muted because the loop opened mid-stream */
	int last_state;		/* last STATE written, -1 unknown; repeats are skipped */
	u32 state_errors, state_mismatches;
	int poll_sample;	/* mute poller's last hook bit, -1 = none since open */
	unsigned long poll_jiffies;
	unsigned long closed_since;	/* muted line seen closed again since (0 = not) */
	u32 open_rx_frame;	/* PCM capture frame counter at stream open */
	u32 poll_mutes, poll_unmutes;
};
static struct slic_dual_line dl[2];

/* Raw SIGREG hook bit of this channel: 1 off-hook, 0 on-hook, <0 ZSI error. */
static int slic_dual_hook_sample(struct slic_dual_line *l)
{
	u8 sig[4];
	int ret = zsi_mpi_read(l->ec, 0x4d, sig, sizeof(sig));

	return ret ? ret : !!(sig[l->ec - 1] & 1);
}

static void slic_dual_mute_arm(void)
{
	/* No-op when already pending; the work re-queues itself while a
	 * stream is open and stops on its own when both are closed. */
	if (READ_ONCE(dual_mute_poll_ms))
		queue_delayed_work(system_highpri_wq, &dual_mute_work, 0);
}

/* A mute flip, logged after sd.lock is released: a console line at 115200
 * baud takes ~10 ms, and the other line's poll and the PCM IRQ would wait. */
struct slic_dual_mute_ev {
	unsigned int line;
	bool mute;
	u32 frames;
};

static void slic_dual_poll_set_mute(struct slic_dual_line *l, bool mute,
				    struct slic_dual_mute_ev *ev)
{
	l->muted = mute;
	pcm_en751221_dual_mute(l->index, mute);
	if (mute)
		l->poll_mutes++;
	else
		l->poll_unmutes++;
	ev->line = l->index + 1;
	ev->mute = mute;
	ev->frames = pcm_en751221_dual_rx_frames() - l->open_rx_frame;
}

static void slic_dual_mute_poll(struct work_struct *w)
{
	unsigned int ms = READ_ONCE(dual_mute_poll_ms);
	struct slic_dual_mute_ev ev[2];
	u8 sig[4], chk[4];
	bool edge = false, checked = false, fast_off = false;
	unsigned long next;
	ktime_t t0;
	s64 us;
	int i, ret, gap, nev = 0;

	if (!ms)
		return;			/* switched off: the next open re-arms */
	if (!mutex_trylock(&sd.lock)) {
		/* A long operation (ring line-up, state verify) owns the bus. */
		dmp.busy++;
		queue_delayed_work(system_highpri_wq, &dual_mute_work,
				   msecs_to_jiffies(2));
		return;
	}
	if (!dl[0].opened && !dl[1].opened) {
		mutex_unlock(&sd.lock);
		return;			/* both streams closed: stop */
	}
	gap = READ_ONCE(zsi_fast_gap_us);
	if (gap <= 0)
		gap = zsi_gap_us;
	t0 = ktime_get();
	ret = zsi_dev_read_fast(0x4d, sig, sizeof(sig), gap);
	if (!ret && sig[0] == 0xff && sig[1] == 0xff && sig[2] == 0xff && sig[3] == 0xff)
		ret = -EIO;		/* Vp886GetEvent's bus-error signature */
	for (i = 0; !ret && i < 2; i++) {
		struct slic_dual_line *l = &dl[i];
		bool open_loop = !(sig[l->ec - 1] & 1);

		if (l->opened && !l->ringing && open_loop != l->muted)
			edge = true;
	}
	if (!ret && edge) {
		ret = zsi_dev_read_fast(0x4d, chk, sizeof(chk), gap);
		checked = !ret;
	}
	us = ktime_us_delta(ktime_get(), t0);
	dmp.last_us = us;
	if (us > dmp.max_us)
		dmp.max_us = us;
	if (ret) {
		dmp.errors++;
		zsi_resync_locked();
		if (++dmp.consecutive >= max(dual_mute_max_errors, 1U) &&
		    READ_ONCE(zsi_fast_gap_us) > 0) {
			WRITE_ONCE(zsi_fast_gap_us, 0);
			dmp.fast_off++;
			fast_off = true;
		}
	} else {
		bool disagree = false;

		dmp.reads++;
		dmp.consecutive = 0;
		for (i = 0; i < 2; i++) {
			struct slic_dual_line *l = &dl[i];
			int v = !!(sig[l->ec - 1] & 1);
			bool open_loop = !v;

			if (l->ringing)
				continue;
			if (checked && !!(chk[l->ec - 1] & 1) != v) {
				if (l->opened && open_loop != l->muted)
					dmp.confirm_fail++;
				disagree = true;
				continue;	/* reads disagree: next sample decides */
			}
			/* The same read serves the idle jack: its manager's hook
			 * reads then use this sample instead of a slow ~15 ms bus
			 * transaction every 150 ms that delayed this poller. */
			l->poll_sample = v;
			l->poll_jiffies = jiffies;
			if (!l->opened)
				continue;	/* idle jack: never muted */
			if (open_loop) {
				l->closed_since = 0;
				if (!l->muted)
					slic_dual_poll_set_mute(l, true, &ev[nev++]);
			} else if (l->muted) {
				if (!l->closed_since)
					l->closed_since = jiffies ?: 1;
				if (time_after_eq(jiffies, l->closed_since +
						  msecs_to_jiffies(READ_ONCE(dual_unmute_ms)))) {
					l->closed_since = 0;
					slic_dual_poll_set_mute(l, false, &ev[nev++]);
				}
			}
		}
		/* Two back-to-back reads that disagree may be one garbled answer
		 * rather than a real edge: realign before the next slow read. */
		if (disagree)
			zsi_resync_locked();
	}
	mutex_unlock(&sd.lock);
	for (i = 0; i < nev; i++)
		pr_info("econet-slic: phone%u capture %s by poller (frame +%u since open, read %lld us)\n",
			ev[i].line, ev[i].mute ? "muted" : "unmuted", ev[i].frames,
			(long long)us);
	if (fast_off)
		pr_warn("econet-slic: mute poller: %u read errors in a row, zsi_fast_gap_us -> 0\n",
			max(dual_mute_max_errors, 1U));
	next = max_t(unsigned long, msecs_to_jiffies(ms),
		     usecs_to_jiffies((unsigned int)min_t(s64, 3 * us, 1000000)));
	queue_delayed_work(system_highpri_wq, &dual_mute_work, next);
}

static int slic_dual_mute_poll_show(struct seq_file *s, void *unused)
{
	int i;

	mutex_lock(&sd.lock);
	seq_printf(s, "poll_ms=%u fast_gap_us=%d zsi_gap_us=%d hook_from_poll=%u unmute_ms=%u pending=%u\n",
		   READ_ONCE(dual_mute_poll_ms), READ_ONCE(zsi_fast_gap_us), zsi_gap_us,
		   READ_ONCE(dual_hook_from_poll), READ_ONCE(dual_unmute_ms),
		   delayed_work_pending(&dual_mute_work));
	seq_printf(s, "reads=%u errors=%u busy=%u confirm_fail=%u fast_off=%u last_us=%lld max_us=%lld\n",
		   dmp.reads, dmp.errors, dmp.busy, dmp.confirm_fail, dmp.fast_off,
		   (long long)dmp.last_us, (long long)dmp.max_us);
	seq_printf(s, "zsi_timeouts=%u zsi_stale_bits=%u\n", zsi_timeouts, zsi_stale_bits);
	for (i = 0; i < 2; i++)
		seq_printf(s, "phone%d opened=%u muted=%u poll_sample=%d mutes=%u unmutes=%u\n",
			   i + 1, dl[i].opened, dl[i].muted, dl[i].poll_sample,
			   dl[i].poll_mutes, dl[i].poll_unmutes);
	mutex_unlock(&sd.lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_dual_mute_poll);

/*
 * Bus check before lowering zsi_fast_gap_us. ONLY with both lines idle (no
 * stream open, no bell): `echo "N GAP" > zsi_selftest` reads RCNPCN (0x73,
 * expected 08 75) N times without the EC prefix and a GAP us settle, then
 * SIGREG once that way and once the slow way (EC + zsi_gap_us), then RCNPCN
 * the slow way to prove the bus is still aligned. Reads only; stops after
 * three errors; result in dmesg. Holds sd.lock for the whole run, so keep N
 * small (<= 200): the managers' hook reads wait meanwhile.
 */
static ssize_t slic_dual_selftest_write(struct file *f, const char __user *ub,
					size_t len, loff_t *off)
{
	unsigned int n, gap, i, bad = 0, errs = 0;
	u8 id[2] = { 0 }, fast[4] = { 0 }, slow[4] = { 0 };
	s64 us, total = 0, max_us = 0;
	u32 ctl, cfg;
	char buf[32];
	ktime_t t0;
	int ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ub, len))
		return -EFAULT;
	buf[len] = '\0';
	if (sscanf(buf, "%u %u", &n, &gap) != 2 || !n || n > 2000 || gap > 5000)
		return -EINVAL;
	mutex_lock(&sd.lock);
	if (dl[0].opened || dl[1].opened || dl[0].ringing || dl[1].ringing) {
		mutex_unlock(&sd.lock);
		return -EBUSY;
	}
	for (i = 0; i < n && errs < 3; i++) {
		t0 = ktime_get();
		ret = zsi_dev_read_fast(0x73, id, sizeof(id), gap);
		us = ktime_us_delta(ktime_get(), t0);
		total += us;
		if (us > max_us)
			max_us = us;
		if (ret || id[0] != 0x08 || id[1] != 0x75) {
			if (!ret)
				bad++;
			errs++;
			zsi_resync_locked();
		}
	}
	ret = zsi_dev_read_fast(0x4d, fast, sizeof(fast), gap);
	if (!ret)
		ret = zsi_mpi_read(VP886_EC_1, 0x4d, slow, sizeof(slow));
	if (!ret)
		ret = zsi_mpi_read(VP886_EC_1, VP886_R_RCNPCN_RD, id, sizeof(id));
	/* Idle wrapper state. The OEM acks with a read-modify-write of CTL
	 * (spi.ko spi_wraper_*: ctl |= bit) where this driver writes the bit
	 * alone: any non-W1C bit set here would be one we keep clearing. */
	ctl = readl(sd.zsi + ZSI_CTL);
	cfg = readl(sd.zsi + ZSI_CFG);
	mutex_unlock(&sd.lock);
	pr_info("econet-slic: zsi selftest idle ctl=0x%08x cfg=0x%08x\n", ctl, cfg);
	pr_info("econet-slic: zsi selftest gap=%uus n=%u done=%u bad=%u errors=%u avg=%lldus max=%lldus\n",
		gap, n, i, bad, errs, i ? (long long)div_s64(total, i) : 0LL,
		(long long)max_us);
	pr_info("econet-slic: zsi selftest sigreg fast=%4ph slow=%4ph hook bits %s; slow rcnpcn=%02x %02x ret=%d\n",
		fast, slow, ((fast[0] ^ slow[0]) | (fast[1] ^ slow[1])) & 1 ? "MISMATCH" : "match",
		id[0], id[1], ret);
	return len;
}

static const struct file_operations slic_dual_selftest_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = slic_dual_selftest_write,
	.llseek = default_llseek,
};

/* Selected-channel register reads only: never invoke legacy reset/probe nodes. */
static int slic_dual_status_show(struct seq_file *s, void *unused)
{
	struct slic_dual_line *l = s->private;
	const u8 opcodes[] = { 0x57, 0x41, 0x43, 0x61, 0x71 };
	const char * const names[] = { "state", "tx_slot", "rx_slot", "opfunc", "opcond" };
	u8 v, icr4[4];
	int i, ret;

	mutex_lock(&sd.lock);
	seq_printf(s, "phone=%u ec=%u ring_requested=%u hook_cache=%d tripped=%u\n",
		l->index + 1, l->ec, l->ringing, l->hook_cache, l->tripped);
	seq_printf(s, "last_state=%d opened=%u state_errors=%u state_mismatches=%u\n",
		l->last_state, l->opened, l->state_errors, l->state_mismatches);
	for (i = 0; i < ARRAY_SIZE(opcodes); i++) {
		ret = zsi_mpi_read(l->ec, opcodes[i], &v, 1);
		if (ret) seq_printf(s, "%s error=%d\n", names[i], ret);
		else seq_printf(s, "%s=0x%02x\n", names[i], v);
	}
	ret = zsi_mpi_read(l->ec, 0xf5, icr4, sizeof(icr4));
	if (!ret)
		seq_printf(s, "icr4=%02x %02x %02x %02x\n", icr4[0], icr4[1], icr4[2], icr4[3]);
	mutex_unlock(&sd.lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_dual_status);

static bool slic_dual_state_matches(u8 actual, u8 target)
{
	/* RE X and metering are status/control bits outside the line-feed mode.
	 * ABS active-high (0x0b) is also a non-ringing active feed. */
	if (target == 0x23)
		return (actual & 0x20) &&
		       ((actual & 0x0f) == 0x03 || (actual & 0x0f) == 0x0b);
	return (actual & 0x0f) == (target & 0x0f);
}

static int slic_dual_verify_state(struct slic_dual_line *l, u8 target)
{
	u8 actual;
	int ret = zsi_mpi_read(l->ec, 0x57, &actual, 1);

	if (ret)
		return ret;
	if (slic_dual_state_matches(actual, target))
		return 0;
	l->state_mismatches++;
	return -EIO;
}

/* sd.lock serializes both managers' complete ZSI operations. Confirm the chip,
 * not just the previous request: failed/ignored commands must remain retryable. */
static int slic_dual_state(struct slic_dual_line *l, u8 state)
{
	u8 cmd[] = { 0x56, state };
	int attempt, ret;

	/* Rewriting the state the line is already in (manager re-activating a
	 * line the stream release left active) would re-run the state entry. */
	if (l->last_state == state && !slic_dual_verify_state(l, state))
		goto confirmed;
	for (attempt = 0; attempt < 3; attempt++) {
		ret = zsi_write(CSLAC_EC_REG_WRT, &l->ec, 1);
		if (!ret)
			ret = slic_write_mpi("dual-state", cmd, sizeof(cmd));
		if (!ret)
			ret = slic_dual_verify_state(l, state);
		if (!ret)
			goto confirmed;
		l->state_errors++;
		l->last_state = -1;
		usleep_range(10000, 12000);
	}
	return ret;
confirmed:
	l->powered = state != 0x0c;
	l->last_state = state;
	l->ringing = (state & 0x0f) == 0x07 || (state & 0x0f) == 0x0a;
	return 0;
}

/* SWCTRL (0xe6) on this line's channel. The switcher is common to both
 * channels: only ever call this while the other jack is in standby. */
static void slic_dual_swctrl(struct slic_dual_line *l, u8 mode)
{
	u8 cmd[] = { 0xe6, mode };

	if (zsi_write(CSLAC_EC_REG_WRT, &l->ec, 1))
		return;
	slic_write_mpi("dual-swctrl", cmd, sizeof(cmd));
}

/* standby -> active feed. See dual_soft_offhook above for the two variants. */
static int slic_dual_feed_up(struct slic_dual_line *l)
{
	struct slic_dual_line *other = &dl[l->index ? 0 : 1];
	int ret;

	if (!dual_soft_offhook || other->powered || other->ringing)
		return slic_dual_state(l, 0x23);

	slic_dual_swctrl(l, 0x65);		/* LP first: soft rail */
	ret = slic_dual_state(l, 0x23);
	usleep_range(20000, 25000);		/* let the loop current settle */
	slic_dual_swctrl(l, 0x6f);		/* back to the proven HP rail */
	return ret;
}

static int slic_dual_open(struct inode *inode, struct file *file)
{
	struct slic_dual_line *l = container_of(file->private_data,
						struct slic_dual_line, misc);
	int ret = pcm_en751221_dual_open(l->index);

	if (ret)
		return ret;
	mutex_lock(&sd.lock);
	ret = slic_dual_state(l, 0x23);
	if (!ret) {
		/* Under sd.lock: the mute poller reads these with it held. */
		l->opened = true;
		l->muted = false;
		l->poll_sample = -1;
		l->closed_since = 0;
		l->open_rx_frame = pcm_en751221_dual_rx_frames();
	}
	mutex_unlock(&sd.lock);
	if (ret) {
		pcm_en751221_dual_close(l->index);
		return ret;
	}
	stream_open(inode, file);
	file->private_data = l;
	slic_dual_mute_arm();
	pr_info("econet-slic: phone%u stream opened (frame %u)\n", l->index + 1,
		l->open_rx_frame);
	return 0;
}

static int slic_dual_release(struct inode *inode, struct file *file)
{
	struct slic_dual_line *l = file->private_data;

	int hook;

	pcm_en751221_dual_close(l->index);
	mutex_lock(&sd.lock);
	l->opened = false;
	l->muted = false;
	l->poll_sample = -1;
	l->closed_since = 0;
	/* Park the line only when the loop is open. With the handset still
	 * lifted (the far end hung up) keep the feed: the standby -> active
	 * bounce the manager did 100 ms later was audible in the handset. */
	hook = slic_dual_hook_sample(l);
	if (hook <= 0)
		slic_dual_state(l, 0x0c);
	mutex_unlock(&sd.lock);
	pr_info("econet-slic: phone%u stream closed (shared PCM retained, %s)\n",
		l->index + 1, hook > 0 ? "loop closed: feed kept" : "parked");
	return 0;
}

static ssize_t slic_dual_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	struct slic_dual_line *l = f->private_data;

	return pcm_en751221_dual_io(l->index, buf, len, true);
}

static ssize_t slic_dual_write(struct file *f, const char __user *buf, size_t len, loff_t *off)
{
	struct slic_dual_line *l = f->private_data;

	return pcm_en751221_dual_io(l->index, (char __user *)buf, len, false);
}

static const struct file_operations slic_dual_fops = {
	.owner = THIS_MODULE, .open = slic_dual_open, .release = slic_dual_release,
	.read = slic_dual_read, .write = slic_dual_write,
};


/* The value the `hook` node reports, with the ring trip side effect. Called
 * with sd.lock held. Returns <0 on a ZSI error (the node then reports it). */
static int slic_dual_hook_read(struct slic_dual_line *l)
{
	int v;

	if (!l->ringing) {
		unsigned int pms = READ_ONCE(dual_mute_poll_ms);
		/* The poller samples idle jacks too while the other line has a
		 * stream open; with no stream it stops and the sample goes stale. */
		bool polled = pms && l->poll_sample >= 0;

		if (polled && READ_ONCE(dual_hook_from_poll) &&
		    time_before(jiffies, l->poll_jiffies + msecs_to_jiffies(2 * pms + 5)))
			v = l->poll_sample;	/* fresh poller sample: no bus traffic */
		else
			v = slic_dual_hook_sample(l);
		if (v < 0)
			return v;
		l->hook_cache = v;
		if (l->tripped && !v)
			l->tripped = false;	/* loop open again: the next ring=1 rings */
		if (pms && l->opened && !delayed_work_pending(&dual_mute_work))
			slic_dual_mute_arm();	/* enabled mid-call, or stopped */
		/* While the poller runs and is not stale it owns the mute (it
		 * holds it through the hang-up contact bounce); otherwise this is
		 * the manager-driven mute as before: first on-hook sample with the
		 * stream open silences the capture (DC-DC buzz to the far end). */
		if (polled && time_before(jiffies, l->poll_jiffies + msecs_to_jiffies(4 * pms + 20)))
			return v;
		if ((l->opened && !v) != l->muted) {
			l->muted = l->opened && !v;
			pcm_en751221_dual_mute(l->index, l->muted);
		}
		return v;
	}
	/* Ringing. Without ring_trip the sample stays frozen at the pre-ring
	 * value (r16 behaviour); with it the SIGREG bit is the ring trip. */
	if (ring_trip) {
		v = slic_dual_hook_sample(l);
		if (v < 0)
			return v;
		if (!v) {
			l->trip_cnt = 0;
		} else if (++l->trip_cnt >= max(ring_trip_reads, 1U)) {
			v = slic_dual_state(l, 0x23);
			if (v)
				return v; /* Do not claim a trip while the chip may ring. */
			l->tripped = true;
			l->trip_cnt = 0;
			l->hook_cache = 1;
			pr_info("econet-slic: phone%u ring trip\n", l->index + 1);
		}
	}
	return l->hook_cache;
}

/* ring=1/0 from the manager. Called with sd.lock held. */
static int slic_dual_ring_write(struct slic_dual_line *l, bool on)
{
	u8 actual;
	int ret;

	if (on) {
		if (l->tripped)
			return 0;	/* no bell until ring=0 */
		ret = slic_ring_state(l->ec, true);
		if (!ret)
			ret = slic_dual_verify_state(l, 0x07);
		if (ret) {
			l->state_errors++;
			l->last_state = -1;
			l->ringing = true; /* Partial command: conservatively needs stop. */
			l->powered = true;
			return ret;
		}
		l->ringing = true;
		l->powered = true;	/* 0x07: never standby */
		l->last_state = 0x07;
		l->trip_cnt = 0;
		return 0;
	}
	/* ring=0 ends the bell only where it is energized. The manager writes
	 * ring=0 to BOTH jacks on every call edge (start, answer, established,
	 * hang-up); r17 turned each of those into STATE 0x23 on the idle jack
	 * too, and on a standby (0x0c) line that is the voltage step the old
	 * DECT base on phone2 rings on for ~3 s ("autosuena", user test 06:5x).
	 * A tripped line is already at 0x23 (written by the trip itself). */
	ret = zsi_mpi_read(l->ec, 0x57, &actual, 1);
	if (ret) {
		l->state_errors++;
		l->last_state = -1;
		return ret;
	}
	/* An already silent idle jack must NOT receive a feed-voltage pulse.
	 * Conversely, a stale false `ringing` flag must not suppress ring-off. */
	if ((actual & 0x0f) == 0x07 || (actual & 0x0f) == 0x0a) {
		ret = slic_dual_state(l, 0x23);
		if (ret)
			return ret;
	} else {
		l->last_state = actual & 0x3f;
		l->powered = (actual & 0x0f) != 0x0c;
	}
	l->ringing = false;
	l->tripped = false;
	l->trip_cnt = 0;
	return 0;
}

static ssize_t slic_dual_control_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	struct slic_dual_line *l = f->private_data;
	const char *name = f->f_path.dentry->d_name.name;
	char value[32];
	int ret = 0, n;

	if (*off)
		return 0;
	mutex_lock(&sd.lock);
	if (!strcmp(name, "hook")) {
		ret = slic_dual_hook_read(l);
		n = scnprintf(value, sizeof(value), "%d\n", ret < 0 ? l->hook_cache : ret);
		if (ret > 0)
			ret = 0;
	} else if (!strcmp(name, "hook_live")) {
		/* Diagnostics only: the raw SIGREG bit, never frozen, no side effect. */
		ret = slic_dual_hook_sample(l);
		n = scnprintf(value, sizeof(value), "%d\n", ret);
		if (ret > 0)
			ret = 0;
	} else if (!strcmp(name, "audio_setup")) {
		ret = slic_dual_state(l, 0x23);
		n = scnprintf(value, sizeof(value), "active=%d\n", !ret);
	} else {
		/* ring and lpm are write-only (0200), but root can still open
		 * them for reading: a read must never change the line state. */
		ret = -EINVAL;
	}
	mutex_unlock(&sd.lock);
	return ret ? ret : simple_read_from_buffer(buf, len, off, value, n);
}

static ssize_t slic_dual_control_write(struct file *f, const char __user *buf, size_t len, loff_t *off)
{
	struct slic_dual_line *l = f->private_data;
	bool ringing = !strcmp(f->f_path.dentry->d_name.name, "ring");
	u8 value;
	int ret = kstrtou8_from_user(buf, len, 10, &value);

	if (ret)
		return ret;
	mutex_lock(&sd.lock);
	if (ringing) {
		ret = slic_dual_ring_write(l, value != 0);
	} else {
		/* Keep the common switcher in HP; never drop another call's rail
		 * (unless dual_soft_offhook is on AND the other jack is idle). */
		ret = value ? slic_dual_state(l, 0x0c) : slic_dual_feed_up(l);
	}
	mutex_unlock(&sd.lock);
	return ret ? ret : len;
}

static const struct file_operations slic_dual_control_fops = {
	.owner = THIS_MODULE, .open = simple_open, .read = slic_dual_control_read,
	.write = slic_dual_control_write, .llseek = default_llseek,
};

static int slic_dual_init(void)
{
	int i, ret, registered = 0;
	int old_ec = slic_ec, old_slot = slic_slot;

	pcm_en751221_voice_reset();
	INIT_DELAYED_WORK(&dual_mute_work, slic_dual_mute_poll);
	dl[0].poll_sample = dl[1].poll_sample = -1;
	mutex_lock(&sd.lock);
	zsi_hw_init();
	zsi_slic_reset();
	slic_load_profiles();
	for (i = 0; i < 2; i++) {
		dl[i].index = i;
		dl[i].ec = i ? VP886_EC_1 : VP886_EC_2;
		dl[i].slot = 4 + 2 * i;
		dl[i].last_state = -1;	/* unknown until the first STATE write */
		slic_ec = dl[i].ec;
		slic_slot = dl[i].slot;
		ret = slic_audio_setup();
		if (ret)
			break;
		slic_lpm_arm_locked();
		ret = slic_dual_state(&dl[i], 0x0c);
		if (ret)
			break;
	}
	slic_ec = old_ec;
	slic_slot = old_slot;
	mutex_unlock(&sd.lock);
	if (ret)
		return ret;
	ret = pcm_en751221_dual_start();
	if (ret)
		return ret;
	sd.dbg = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("mute_poll", 0400, sd.dbg, NULL, &slic_dual_mute_poll_fops);
	debugfs_create_file("zsi_selftest", 0200, sd.dbg, NULL, &slic_dual_selftest_fops);
	for (i = 0; i < 2; i++) {
		struct dentry *dir;

		dl[i].misc.minor = MISC_DYNAMIC_MINOR;
		dl[i].misc.name = i ? "xr500v-voice1" : "xr500v-voice";
		dl[i].misc.fops = &slic_dual_fops;
		ret = misc_register(&dl[i].misc);
		if (ret)
			goto fail;
		registered++;
		dir = debugfs_create_dir(i ? "phone2" : "phone1", sd.dbg);
		debugfs_create_file("status", 0400, dir, &dl[i], &slic_dual_status_fops);
		debugfs_create_file("hook", 0400, dir, &dl[i], &slic_dual_control_fops);
		debugfs_create_file("hook_live", 0400, dir, &dl[i], &slic_dual_control_fops);
		debugfs_create_file("audio_setup", 0400, dir, &dl[i], &slic_dual_control_fops);
		debugfs_create_file("ring", 0200, dir, &dl[i], &slic_dual_control_fops);
		debugfs_create_file("lpm", 0200, dir, &dl[i], &slic_dual_control_fops);
	}
	return 0;
fail:
	while (registered--)
		misc_deregister(&dl[registered].misc);
	debugfs_remove_recursive(sd.dbg);
	pcm_en751221_dual_stop();
	return ret;
}

static void slic_dual_exit(void)
{
	int i;

	/* No stream can be open here (the misc fops pin the module), so the
	 * poller is idle or about to stop; trylock never blocks on us. */
	cancel_delayed_work_sync(&dual_mute_work);
	pcm_en751221_dual_stop();
	for (i = 0; i < 2; i++) {
		misc_deregister(&dl[i].misc);
		mutex_lock(&sd.lock);
		slic_dual_state(&dl[i], 0x0c);
		mutex_unlock(&sd.lock);
	}
	debugfs_remove_recursive(sd.dbg);
}

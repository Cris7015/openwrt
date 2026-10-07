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
};
static struct slic_dual_line dl[2];

/* Raw SIGREG hook bit of this channel: 1 off-hook, 0 on-hook, <0 ZSI error. */
static int slic_dual_hook_sample(struct slic_dual_line *l)
{
	u8 sig[4];
	int ret = zsi_mpi_read(l->ec, 0x4d, sig, sizeof(sig));

	return ret ? ret : !!(sig[l->ec - 1] & 1);
}

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
	mutex_unlock(&sd.lock);
	if (ret) {
		pcm_en751221_dual_close(l->index);
		return ret;
	}
	stream_open(inode, file);
	file->private_data = l;
	l->opened = true;
	l->muted = false;
	pr_info("econet-slic: phone%u stream opened\n", l->index + 1);
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
		v = slic_dual_hook_sample(l);
		if (v < 0)
			return v;
		l->hook_cache = v;
		if (l->tripped && !v)
			l->tripped = false;	/* loop open again: the next ring=1 rings */
		/* First on-hook sample with the stream open: silence the capture
		 * now, not after the manager's debounce (DC-DC buzz to the far end). */
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

	pcm_en751221_dual_stop();
	for (i = 0; i < 2; i++) {
		misc_deregister(&dl[i].misc);
		mutex_lock(&sd.lock);
		slic_dual_state(&dl[i], 0x0c);
		mutex_unlock(&sd.lock);
	}
	debugfs_remove_recursive(sd.dbg);
}

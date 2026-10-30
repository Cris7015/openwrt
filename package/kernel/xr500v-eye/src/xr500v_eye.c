// SPDX-License-Identifier: GPL-2.0
/*
 * Lab-only eye monitor for the EN751221 xPON PHY (0x1faf0000).
 *
 * Follows the stock phy.ko phy_eye_monitor() without its pon_phy_deinit()
 * call: it only moves the eye sampler (phase 0x12c[30:24], threshold
 * 0x118[6:0]), triggers one count per point (0x110[12], done in 0x13c[10],
 * 20-bit count in 0x148) and restores every field it touched. Phase moves
 * are ramped like the stock code does.
 *
 * debugfs xr500v-eye/:
 *   peek    write an offset from the allowlist; read it back once
 *   point   write "h v dur": one point, h and v in 0..127 (64 = centre)
 *   scan    write "step dur": the whole grid every <step> codes
 *   result  "h v count" lines of the last point or scan
 */
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#define PHY_BASE	0x1faf0000
#define PHY_SIZE	0x600

#define PHYSET5		0x110
#define  EYE_EN		BIT(11)
#define  EYE_START	BIT(12)
#define PHYSET7		0x118
#define  EYE_VTH	GENMASK(6, 0)
#define PHYSET8		0x11c
#define  EYE_DUR	GENMASK(15, 0)
#define  EYE_PI_ROUTE	BIT(27)	/* vendor PHY_PILPO_ROUTE: set, never cleared */
#define PHYSET12	0x12c
#define  EYE_PHASE	GENMASK(30, 24)
#define PHYSTA4		0x13c
#define  EYE_DONE	BIT(10)
#define PHYSTA7		0x148
#define  EYE_CNT	GENMASK(19, 0)

#define EYE_N		128
#define EYE_POLL_US	20000

/* Never 0x5e0/0x5f8: reading those hangs the SoC. */
static const u16 peek_ok[] = {
	0x110, 0x118, 0x11c, 0x12c, 0x130, 0x13c, 0x148, 0x14c, 0x2d4
};

static void __iomem *base;
static struct dentry *dir;
static DEFINE_MUTEX(eye_lock);
static u32 *res;		/* EYE_N * EYE_N counts, U32_MAX = not measured */
static u32 peek_off, peek_val;
static bool peek_valid;
static unsigned int stale_done, align_tries, cur_dur;

static unsigned int settle_us = 20;
module_param(settle_us, uint, 0644);
static unsigned int align_max;
module_param(align_max, uint, 0644);
static unsigned int phase_step = 1;
module_param(phase_step, uint, 0644);
static unsigned int phase_us = 50;
module_param(phase_us, uint, 0644);

struct eye_saved {
	u32 set5, set7, set8, set12;
};

static u32 rd(u32 off)
{
	return readl(base + off);
}

static void wr(u32 off, u32 val)
{
	writel(val, base + off);
}

static void rmw(u32 off, u32 mask, u32 val)
{
	wr(off, (rd(off) & ~mask) | (val & mask));
}

/* 7-bit two's complement code <-> signed offset */
static int code2s(u32 code)
{
	return (code & 0x40) ? (int)code - 128 : (int)code;
}

static u32 s2code(int s)
{
	return (u32)s & 0x7f;
}

static void set_phase(int target)
{
	int cur = code2s(FIELD_GET(EYE_PHASE, rd(PHYSET12)));

	while (cur != target) {
		int step = clamp_t(int, phase_step, 1, 16);

		cur += clamp(target - cur, -step, step);
		rmw(PHYSET12, EYE_PHASE, FIELD_PREP(EYE_PHASE, s2code(cur)));
		udelay(phase_us);
	}
}

static void eye_begin(struct eye_saved *s, u32 dur)
{
	s->set5 = rd(PHYSET5);
	s->set7 = rd(PHYSET7);
	s->set8 = rd(PHYSET8);
	s->set12 = rd(PHYSET12);
	pr_info("xr500v-eye: begin set5=%08x set7=%08x set8=%08x set12=%08x dur=%u\n",
		s->set5, s->set7, s->set8, s->set12, dur);
	cur_dur = dur;
	wr(PHYSET8, (s->set8 & ~EYE_DUR) | FIELD_PREP(EYE_DUR, dur) | EYE_PI_ROUTE);
	usleep_range(1000, 2000);
	rmw(PHYSET5, EYE_EN, EYE_EN);
}

static void eye_end(const struct eye_saved *s)
{
	/*
	 * Like the vendor: bring the phase back with the monitor still
	 * enabled and leave PHY_PILPO_ROUTE set. Returning it with EN=0, or
	 * dropping the route, slips the eye comparison by a bit.
	 */
	set_phase(code2s(FIELD_GET(EYE_PHASE, s->set12)));
	rmw(PHYSET5, EYE_START | EYE_EN, s->set5);
	rmw(PHYSET7, EYE_VTH, s->set7);
	rmw(PHYSET8, EYE_DUR, s->set8);
	pr_info("xr500v-eye: end set5=%08x set7=%08x set8=%08x set12=%08x stale_done=%u\n",
		rd(PHYSET5), rd(PHYSET7), rd(PHYSET8), rd(PHYSET12), stale_done);
}

/* h, v: signed offsets -64..63 */
static int measure(int h, int v, u32 *cnt)
{
	u32 sta;
	int ret;

	/* The done bit stays set after a point; only count how often. */
	if (rd(PHYSTA4) & EYE_DONE)
		stale_done++;

	set_phase(h);
	rmw(PHYSET7, EYE_VTH, s2code(v));
	udelay(settle_us);
	rmw(PHYSET5, EYE_START, EYE_START);
	udelay(2);
	ret = readl_poll_timeout(base + PHYSTA4, sta, sta & EYE_DONE, 10,
				 EYE_POLL_US);
	if (!ret) {
		/* done is sticky: wait out dur (8-bit words at 3.2 ns) as well */
		udelay(cur_dur * 32 / 10000 + 1);
		rd(PHYSTA7);
		*cnt = FIELD_GET(EYE_CNT, rd(PHYSTA7));
	}
	rmw(PHYSET5, EYE_START, 0);

	return ret;
}

/*
 * The eye samples are compared with the data decisions through a pipeline
 * whose alignment is picked when the monitor is enabled: misaligned by a
 * bit, even the eye centre counts ~50 % (4 * dur). Re-enable the monitor
 * until the centre is clean, then keep it enabled for the whole sweep.
 */
static int eye_align(u32 dur)
{
	u32 cnt;
	int ret;

	if (!align_max)
		return 0;
	for (align_tries = 1; align_tries <= align_max; align_tries++) {
		ret = measure(0, 0, &cnt);
		if (ret)
			return ret;
		if (cnt <= dur / 8)
			return 0;
		rmw(PHYSET5, EYE_EN, 0);
		udelay(100);
		rmw(PHYSET5, EYE_EN, EYE_EN);
		udelay(100);
	}

	return -EAGAIN;
}

static ssize_t realign_write(struct file *f, const char __user *ubuf,
			     size_t len, loff_t *ppos)
{
	struct eye_saved s;
	char buf[24];
	u32 cnt = 0;
	int k, ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	if (kstrtoint(strim(buf), 0, &k) || k < -64 || k > 63)
		return -EINVAL;

	mutex_lock(&eye_lock);
	memset32(res, U32_MAX, EYE_N * EYE_N);
	eye_begin(&s, 4096);
	rmw(PHYSET5, EYE_EN, 0);
	set_phase(k);
	rmw(PHYSET5, EYE_EN, EYE_EN);
	set_phase(0);
	ret = measure(0, 0, &cnt);
	eye_end(&s);
	if (!ret)
		res[64 * EYE_N + 64] = cnt;
	pr_info("xr500v-eye: realign via %d -> %d cnt=%u\n", k, ret, cnt);
	mutex_unlock(&eye_lock);

	return ret ?: len;
}

static const struct file_operations realign_fops = {
	.write = realign_write,
	.llseek = noop_llseek,
};

static ssize_t peek_write(struct file *f, const char __user *ubuf,
			  size_t len, loff_t *ppos)
{
	unsigned int i;
	u32 off;
	int ret;

	ret = kstrtou32_from_user(ubuf, len, 0, &off);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(peek_ok); i++)
		if (peek_ok[i] == off)
			break;
	if (i == ARRAY_SIZE(peek_ok))
		return -EPERM;

	mutex_lock(&eye_lock);
	pr_info("xr500v-eye: peek %#x\n", off);
	peek_off = off;
	peek_val = rd(off);
	peek_valid = true;
	pr_info("xr500v-eye: peek %#x = %08x\n", off, peek_val);
	mutex_unlock(&eye_lock);

	return len;
}

static ssize_t peek_read(struct file *f, char __user *ubuf, size_t len,
			 loff_t *ppos)
{
	char buf[48];
	int n;

	mutex_lock(&eye_lock);
	n = peek_valid ? scnprintf(buf, sizeof(buf), "off=%#x val=%#010x\n",
				   peek_off, peek_val) :
			 scnprintf(buf, sizeof(buf), "none\n");
	mutex_unlock(&eye_lock);

	return simple_read_from_buffer(ubuf, len, ppos, buf, n);
}

static const struct file_operations peek_fops = {
	.write = peek_write,
	.read = peek_read,
	.llseek = default_llseek,
};

static int args_from_user(const char __user *ubuf, size_t len, u32 *a, int n)
{
	char buf[64];
	int got;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	got = (n == 3) ? sscanf(buf, "%u %u %u", &a[0], &a[1], &a[2]) :
			 sscanf(buf, "%u %u", &a[0], &a[1]);

	return (got == n) ? 0 : -EINVAL;
}

static ssize_t point_write(struct file *f, const char __user *ubuf,
			   size_t len, loff_t *ppos)
{
	struct eye_saved s;
	u32 a[3], cnt = 0;
	int ret;

	ret = args_from_user(ubuf, len, a, 3);
	if (ret)
		return ret;
	if (a[0] >= EYE_N || a[1] >= EYE_N || !a[2] || a[2] > 0xffff)
		return -EINVAL;

	mutex_lock(&eye_lock);
	memset32(res, U32_MAX, EYE_N * EYE_N);
	stale_done = 0;
	eye_begin(&s, a[2]);
	ret = eye_align(a[2]);
	if (!ret)
		ret = measure((int)a[0] - 64, (int)a[1] - 64, &cnt);
	eye_end(&s);
	if (!ret)
		res[a[0] * EYE_N + a[1]] = cnt;
	pr_info("xr500v-eye: point h=%u v=%u dur=%u -> %d cnt=%u align_tries=%u\n",
		a[0], a[1], a[2], ret, cnt, align_tries);
	mutex_unlock(&eye_lock);

	return ret ?: len;
}

static const struct file_operations point_fops = {
	.write = point_write,
	.llseek = noop_llseek,
};

static ssize_t scan_write(struct file *f, const char __user *ubuf,
			  size_t len, loff_t *ppos)
{
	struct eye_saved s;
	u32 a[2], cnt;
	int h, v, ret;

	ret = args_from_user(ubuf, len, a, 2);
	if (ret)
		return ret;
	if (!a[0] || a[0] > 64 || !a[1] || a[1] > 0xffff)
		return -EINVAL;

	mutex_lock(&eye_lock);
	memset32(res, U32_MAX, EYE_N * EYE_N);
	stale_done = 0;
	eye_begin(&s, a[1]);
	ret = eye_align(a[1]);
	for (h = -64; h < 64 && !ret; h += a[0]) {
		for (v = -64; v < 64; v += a[0]) {
			ret = measure(h, v, &cnt);
			if (ret)
				break;
			res[(h + 64) * EYE_N + (v + 64)] = cnt;
			cond_resched();
		}
	}
	eye_end(&s);
	pr_info("xr500v-eye: scan step=%u dur=%u -> %d align_tries=%u\n",
		a[0], a[1], ret, align_tries);
	mutex_unlock(&eye_lock);

	return ret ?: len;
}

/*
 * Sweep outwards from the eye centre: right half (phase 0 -> +63), back to
 * the centre, left half (0 -> -64). Past the eye edge the comparison may
 * slip a bit, which does not matter there (the eye is closed), and a
 * short move back inside realigns it before the second half.
 */
static ssize_t scanx_write(struct file *f, const char __user *ubuf,
			   size_t len, loff_t *ppos)
{
	struct eye_saved s;
	u32 a[3] = { 0, 0, 0 }, cnt, centre[2] = { U32_MAX, U32_MAX };
	int dir, h, v, ret;
	char buf[64];

	/* "step dur [halves]": halves 0 = both, 1 = right only, 2 = left only */
	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	if (sscanf(buf, "%u %u %u", &a[0], &a[1], &a[2]) < 2)
		return -EINVAL;
	if (!a[0] || a[0] > 64 || !a[1] || a[1] > 0xffff || a[2] > 2)
		return -EINVAL;
	ret = 0;

	mutex_lock(&eye_lock);
	memset32(res, U32_MAX, EYE_N * EYE_N);
	stale_done = 0;
	eye_begin(&s, a[1]);
	for (dir = 0; dir < 2 && !ret; dir++) {
		if ((a[2] == 1 && dir) || (a[2] == 2 && !dir))
			continue;
		/* back inside the eye from wherever the last half ended */
		if (a[2] == 0) {
			set_phase(-8);
			set_phase(0);
		}
		ret = measure(0, 0, &centre[dir]);
		for (h = 0; !ret && h >= -64 && h <= 63;
		     h += dir ? -(int)a[0] : (int)a[0]) {
			if (dir && !h)
				continue;
			for (v = -64; v < 64; v += a[0]) {
				ret = measure(h, v, &cnt);
				if (ret)
					break;
				res[(h + 64) * EYE_N + (v + 64)] = cnt;
				cond_resched();
			}
		}
	}
	set_phase(-8);
	eye_end(&s);
	pr_info("xr500v-eye: scanx step=%u dur=%u -> %d centre=%u/%u\n",
		a[0], a[1], ret, centre[0], centre[1]);
	mutex_unlock(&eye_lock);

	return ret ?: len;
}

static const struct file_operations scanx_fops = {
	.write = scanx_write,
	.llseek = noop_llseek,
};

static const struct file_operations scan_fops = {
	.write = scan_write,
	.llseek = noop_llseek,
};

static int result_show(struct seq_file *m, void *v)
{
	unsigned int i;

	mutex_lock(&eye_lock);
	for (i = 0; i < EYE_N * EYE_N; i++)
		if (res[i] != U32_MAX)
			seq_printf(m, "%u %u %u\n", i / EYE_N, i % EYE_N, res[i]);
	mutex_unlock(&eye_lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(result);

static int __init xr500v_eye_init(void)
{
	res = vmalloc(array_size(EYE_N * EYE_N, sizeof(*res)));
	if (!res)
		return -ENOMEM;
	memset32(res, U32_MAX, EYE_N * EYE_N);

	base = ioremap(PHY_BASE, PHY_SIZE);
	if (!base) {
		vfree(res);
		return -ENOMEM;
	}

	dir = debugfs_create_dir("xr500v-eye", NULL);
	debugfs_create_file("peek", 0600, dir, NULL, &peek_fops);
	debugfs_create_file("point", 0200, dir, NULL, &point_fops);
	debugfs_create_file("scan", 0200, dir, NULL, &scan_fops);
	debugfs_create_file("result", 0400, dir, NULL, &result_fops);
	debugfs_create_file("realign", 0200, dir, NULL, &realign_fops);
	debugfs_create_file("scanx", 0200, dir, NULL, &scanx_fops);
	pr_info("xr500v-eye: loaded (lab only)\n");

	return 0;
}

static void __exit xr500v_eye_exit(void)
{
	debugfs_remove(dir);
	iounmap(base);
	vfree(res);
	pr_info("xr500v-eye: unloaded\n");
}

module_init(xr500v_eye_init);
module_exit(xr500v_eye_exit);
MODULE_DESCRIPTION("Lab-only eye monitor for the EN751221 xPON PHY");
MODULE_LICENSE("GPL");

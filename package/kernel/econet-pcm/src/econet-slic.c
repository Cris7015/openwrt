// SPDX-License-Identifier: GPL-2.0
/*
 * EcoNet/TrendChip EN751221 Le9642 SLIC ZSI driver -- phase 3a (chip detect).
 *
 * The XR500v wires its two Le9642 SLICs over ZSI (Zarlink Serial Interface),
 * NOT plain SPI -- the OEM does `insmod slic3.ko type=ZSI`. ZSI multiplexes the
 * SLIC control channel over the PCM bus: control bytes go through a ZSI wrapper
 * at 0x1fbd1000 (+id*0x2000), while the PCM engine must be generating PCLK/FSYNC
 * for the SLIC to clock its responses. So this driver drives the ZSI wrapper
 * AND brings the PCM up in ZSI clock mode.
 *
 * Reconstructed from spi.ko (ZSI_bytes_read/write), slic3_main.c (the type=ZSI
 * init sequence) and pcm1.ko (timeSlotCfgReinit). The ZSI wrapper handshake was
 * verified live by register-poking before this driver was written: the wrapper
 * ack (bit1) and rx-ready (bit2) bits respond correctly; the remaining piece is
 * the PCM clock, which is what this module adds.
 *
 * Detect runs on demand via debugfs (opt-in, re-runnable). The PCM clock
 * timeslot table and INTFACE_CTRL are debugfs-writable so they can be swept on
 * the live device without reflashing.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define DRV_NAME "econet-slic"

/* MMIO bases (phys). */
#define CHIP_SCU_PHYS		0x1fa20000	/* pinmux/clock */
#define CHIP_SCU_SIZE		0x1000
#define SYS_PHYS		0x1fb00000	/* SCU: chip-id, spi-mode, gpio/reset */
#define SYS_SIZE		0x1000
#define PCM_PHYS		0x1fbd0000	/* PCM engine (clock/fsync for ZSI) */
#define PCM_SIZE		0x1000
#define ZSI_PHYS		0x1fbd1000	/* ZSI wrapper (+ id*0x2000) */
#define ZSI_SIZE		0x1000

/* chip_scu offsets */
#define SCU_IOMUX_CONTROL1	0x104
#define SCU_PCM_CLK_SRC_SEL	0x148
#define IOMUX1_SLIC_MASK	0x7d00
#define IOMUX1_GPIO_ZSI_ISI	0x2000	/* ZSI/ISI pin routing (vs 0x7400 SPI) */
#define PCM_ZSI_CLK_SOURCE	0x0c	/* PCM_CLK_SOURCE_SELECT bits[3:2]=1 */

/* sys/SCU offsets */
#define SYS_CHIP_ID		0x064
#define SYS_SPI_MODE		0x094	/* [3:0] interface mode; ZSI route = 0x5 */
#define SYS_GPIO_DATA		0x834	/* SLIC reset: bit0 + bit17 (NOT bit25) */
#define SYS_CHIP_EN7526C	0x00080000
#define SYS_MODE_ZSI		0x5

/* PCM engine registers (same map as econet-pcm). */
#define PCM_INTFACE_CTRL	0x00
#define PCM_TX_TIME_SLOT_CFG0	0x04	/* 0x04..0x10 */
#define PCM_RX_TIME_SLOT_CFG0	0x14	/* 0x14..0x20 */

/* ZSI wrapper registers (0x1fbd1000 + id*0x2000). */
#define ZSI_CFG			0x000	/* (old & 0xe0ffffef) | 0x0f000010 */
#define ZSI_TXRX		0x004	/* write cmd/data byte */
#define ZSI_CTL			0x008	/* bit0 read-req, bit1 tx-ack, bit2 rx-ready */
#define ZSI_RX			0x00c	/* received byte (low 8 bits) */
#define ZSI_EN			0x010	/* block enable = 0x19 */

#define ZSI_CFG_MASK		0xe0ffffefu
#define ZSI_CFG_VAL		0x0f030313u	/* OEM-stock observed value (voip running) */
#define ZSI_EN_VAL		0x19
#define ZSI_CTL_READ_REQ	BIT(0)
#define ZSI_CTL_TX_ACK		BIT(1)
#define ZSI_CTL_RX_READY	BIT(2)

/* MPI / Le9642 (VP886) constants. */
#define CSLAC_EC_REG_WRT	0x4a
#define VP886_EC_1		0x01
#define VP886_EC_2		0x02
#define VP886_R_RCNPCN_RD	0x73
#define VP886_R_RCNPCN_LEN	2
#define VP886_R_RCNPCN_RCN	0x08
#define VP886_R_RCNPCN_PCN_LE9642 0x75	/* le9662.c: le9642 = rcn 0x08 / pcn 0x75 */

#define ZSI_POLL_ITERS		20000	/* x1us = up to 20ms per wait */

struct slic_dev {
	void __iomem *chip_scu;
	void __iomem *sys;
	void __iomem *pcm;
	void __iomem *zsi;
	struct dentry *dbg;
	struct mutex lock;
	u8 cs;
	const char *where;	/* phase tag for timeout reporting */
};

static struct slic_dev sd;

/*
 * PCM clock config for ZSI mode. These set the PCM engine generating PCLK/FSYNC
 * so the SLIC has a clock. timeSlotCfgReinit(2) in the OEM computes the slot
 * regs; INTFACE_CTRL must enable the engine WITHOUT loopback (clock goes to the
 * external pins, not internal). Debugfs-writable to sweep on live HW.
 *
 * Defaults: INTFACE_CTRL without loopback (bit25 clear) + config-valid (bit31);
 * timeslots seeded from the 2b loopback values. CONFIRM against an OEM-stock
 * register dump (voip running) -- these are the best estimate pre-dump.
 */
/* Values read from the OEM-stock device with voip running (ground truth). */
static u32 pcm_intface_ctrl = 0xf5071306;
static u32 pcm_tx_slots[4] = { 0x10301020, 0x10501040, 0x10701060, 0x10901080 };
static u32 pcm_rx_slots[4] = { 0x10301020, 0x10501040, 0x10701060, 0x10901080 };
static u32 zsi_cfg_val = ZSI_CFG_VAL;	/* debugfs-tunable */

static int zsi_poll_set(u32 mask)
{
	int i;

	for (i = 0; i < ZSI_POLL_ITERS; i++) {
		if (readl(sd.zsi + ZSI_CTL) & mask)
			return 0;
		udelay(1);
	}
	pr_err(DRV_NAME ": TIMEOUT @%s waiting ZSI_CTL&0x%x (ctl=0x%08x)\n",
	       sd.where, mask, readl(sd.zsi + ZSI_CTL));
	return -ETIMEDOUT;
}

/*
 * Inter-byte ZSI settle gap (microseconds). The SLIC needs time to clock each
 * byte over the 8 kHz PCM bus before the next or it replies garbage (0xf1).
 * 5000 is the safe stock value, but ~300 profile bytes * 5 ms = ~1.5 s of the
 * call-answer line-up (the dominant "connected but silent" delay). Lower it
 * live (echo N > /sys/module/econet_slic/parameters/zsi_gap_us) to cut that
 * latency -- the profile load is fire-and-forget so a too-short gap mis-loads
 * silently; validate RCN/PCN readback (08 75) + a clean sustained call at each
 * step before keeping a lower floor.
 */
static int zsi_gap_us = 5000;
module_param(zsi_gap_us, int, 0644);

/* ZSI: write one byte (cmd or data). Verified-by-poke handshake. */
static int zsi_write_byte(u8 v)
{
	int ret;

	writel(v, sd.zsi + ZSI_TXRX);
	ret = zsi_poll_set(ZSI_CTL_TX_ACK);
	if (ret)
		return ret;
	writel(ZSI_CTL_TX_ACK, sd.zsi + ZSI_CTL);	/* W1C the ack */
	/* the wrapper ack only means the wrapper sent; the SLIC needs time to
	 * clock the byte over the PCM bus (8 kHz frames). Without this gap the
	 * SLIC replies garbage (0xf1). 5 ms verified live; ~125 us/frame so safe.
	 * Tunable via zsi_gap_us to cut the ~1.5 s profile-load answer latency. */
	usleep_range(zsi_gap_us, zsi_gap_us + 1000);
	return 0;
}

/* ZSI: clock in and read one byte. */
static int zsi_read_byte(u8 *out)
{
	int ret;

	writel(ZSI_CTL_READ_REQ, sd.zsi + ZSI_CTL);
	ret = zsi_poll_set(ZSI_CTL_RX_READY);
	if (ret)
		return ret;
	*out = readl(sd.zsi + ZSI_RX) & 0xff;
	writel(ZSI_CTL_RX_READY, sd.zsi + ZSI_CTL);	/* W1C the ready */
	usleep_range(zsi_gap_us, zsi_gap_us + 1000);	/* inter-byte gap (see zsi_write_byte) */
	return 0;
}

static void zsi_begin(void)
{
	/* OEM stock shows ZSI_CFG = 0x0f030313 with voip running; write it. */
	writel(zsi_cfg_val, sd.zsi + ZSI_CFG);
}

static int zsi_write(u8 cmd, const u8 *buf, u8 len)
{
	int ret;
	u8 i;

	sd.where = "zsi_write";
	zsi_begin();
	ret = zsi_write_byte(cmd);
	if (ret)
		return ret;
	for (i = 0; i < len; i++) {
		ret = zsi_write_byte(buf[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int zsi_read(u8 cmd, u8 *buf, u8 len)
{
	int ret;
	u8 i;

	sd.where = "zsi_read";
	zsi_begin();
	ret = zsi_write_byte(cmd);
	if (ret)
		return ret;
	/* ZSI read sub-command: consumes the framing byte so the first read
	 * byte is the real register data (verified live: 0x06 aligns RCN=0x08). */
	ret = zsi_write_byte(0x06);
	if (ret)
		return ret;
	for (i = 0; i < len; i++) {
		ret = zsi_read_byte(&buf[i]);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Enable the PCM clock output (SCU 0x1fa200d8). The ZSI control read does NOT
 * need the PCM DMA -- the ZSI wrapper self-clocks. Live testing showed that
 * arming the DMA actually PREVENTED the SLIC from answering (the SLIC replied
 * only with NO DMA running). So we just enable the PCLK output; no rings.
 */
#define SCU_PCM_CLK_OUTPUT	0x0d8
#define SCU_PCM_CLK_OUTPUT_VAL	0x00a00301	/* OEM-stock value */

/*
 * Configure the PCM as master so it generates PCLK/FSYNC for the SLIC:
 * clock output enable + the OEM timeslot table + INTFACE_CTRL (0xf5071306,
 * master mode, no loopback). NO DMA -- arming the DMA was what blocked the
 * SLIC; only this clock-master config is needed for the ZSI control channel.
 */
static void pcm_zsi_clock_setup(void)
{
	int i;

	writel(SCU_PCM_CLK_OUTPUT_VAL, sd.chip_scu + SCU_PCM_CLK_OUTPUT);
	for (i = 0; i < 4; i++) {
		writel(pcm_tx_slots[i], sd.pcm + PCM_TX_TIME_SLOT_CFG0 + i * 4);
		writel(pcm_rx_slots[i], sd.pcm + PCM_RX_TIME_SLOT_CFG0 + i * 4);
	}
	writel(pcm_intface_ctrl, sd.pcm + PCM_INTFACE_CTRL);
}

/* type=ZSI init from slic3_main.c: route, iomux GPIO_ZSI_ISI, ZSI clock src. */
static void zsi_hw_init(void)
{
	u32 v;

	/* interface route = ZSI (0x1fb00094 [3:0] = 0x5) */
	v = readl(sd.sys + SYS_SPI_MODE);
	writel((v & ~0xfu) | SYS_MODE_ZSI, sd.sys + SYS_SPI_MODE);

	/* pinmux: GPIO_ZSI_ISI (clear SLIC field, set ZSI) */
	v = readl(sd.chip_scu + SCU_IOMUX_CONTROL1);
	writel((v & ~IOMUX1_SLIC_MASK) | IOMUX1_GPIO_ZSI_ISI,
	       sd.chip_scu + SCU_IOMUX_CONTROL1);

	/* PCM clock source = ZSI (OEM stock low nibble = 0x1c, not just 0x0c) */
	v = readl(sd.chip_scu + SCU_PCM_CLK_SRC_SEL);
	writel(v | 0x1c, sd.chip_scu + SCU_PCM_CLK_SRC_SEL);

	/* PCM clock running (PCLK/FSYNC for the SLIC) */
	pcm_zsi_clock_setup();

	/* ZSI wrapper block enable + config */
	writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
}

/* ZSI SLIC reset: 0x1fb00834 bit0 then bit17 (EN751221). NOT bit25 (that's SPI). */
static void zsi_slic_reset(void)
{
	u32 g, chip;

	g = readl(sd.sys + SYS_GPIO_DATA);
	writel(g | BIT(0), sd.sys + SYS_GPIO_DATA);
	msleep(5);
	writel(g & ~BIT(0), sd.sys + SYS_GPIO_DATA);

	chip = readl(sd.sys + SYS_CHIP_ID) & 0xffff0000;
	if (chip != SYS_CHIP_EN7526C) {
		g = readl(sd.sys + SYS_GPIO_DATA);
		writel(g | BIT(17), sd.sys + SYS_GPIO_DATA);
		msleep(5);
		writel(g & ~BIT(17), sd.sys + SYS_GPIO_DATA);
	}
	msleep(10);	/* let the SLIC core boot after reset */
}

/* MPI register read over ZSI: write [EC_REG_WRT, ec] then read `len` of `reg`. */
static int zsi_mpi_read(u8 ec, u8 reg, u8 *buf, u8 len)
{
	int ret;

	ret = zsi_write(CSLAC_EC_REG_WRT, &ec, 1);
	if (ret)
		return ret;
	return zsi_read(reg, buf, len);
}

static int slic_detect(u8 *buf, u8 n)
{
	int ret;

	mutex_lock(&sd.lock);
	zsi_hw_init();
	zsi_slic_reset();
	/* re-assert wrapper config after reset */
	writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
	ret = zsi_mpi_read(VP886_EC_1, VP886_R_RCNPCN_RD, buf, n);
	mutex_unlock(&sd.lock);
	return ret;
}

/*
 * Phase 3b: device profile (Raw MPI section of DEV_PROFILE_100V_BB_124_ZSI).
 * Each entry is an MPI command (opcode + data bytes) written straight to the
 * SLIC over ZSI. Validated by poking: written from the good (post-detect)
 * state with NO HWRESET, the SLIC survives and the Device Mode reads back the
 * written value. The 0xf6/0xe4/0xe6 commands start the buck-boost switcher.
 */
/*
 * Raw MPI sections of the ZLR964124_Le9641 BB profiles (ZSI mode), written
 * straight over ZSI. Each opcode self-delimits (the SLIC parses the stream),
 * so we just stream the bytes. All validated by poking (SLIC survives, device
 * mode reads back, AC 80-byte command accepted).
 */
static const u8 dev_mpi[] = {	/* DEV_PROFILE_100V_BB_124_ZSI: PCLK, slot, dev mode, switcher */
	0x46, 0x02, 0x44, 0x06, 0x5e, 0x14, 0x00, 0xf6, 0x95, 0x00,
	0x58, 0x30, 0x5c, 0x30, 0xe4, 0x44, 0x92, 0x0a, 0xe6, 0x60,
};
static const u8 dc_mpi[] = {	/* DC_FXS_miSLIC_BB_DEF: DC feed */
	0xc6, 0x92, 0x27,
};
static const u8 ac_mpi[] = {	/* AC_FXS_RF14_600R_DEF_LE9641: AC impedance coeffs */
	0xa4, 0x00, 0xf4, 0x4c, 0x01, 0x49, 0xca, 0xf5, 0x98, 0xaa, 0x7b, 0xab,
	0x2c, 0xa3, 0x25, 0xa5, 0x24, 0xb2, 0x3d, 0x9a, 0x2a, 0xaa, 0xa6, 0x9f,
	0x01, 0x8a, 0x1d, 0x01, 0xa3, 0xa0, 0x2e, 0xb2, 0xb2, 0xba, 0xac, 0xa2,
	0xa6, 0xcb, 0x3b, 0x45, 0x88, 0x2a, 0x20, 0x3c, 0xbc, 0x4e, 0xa6, 0x2b,
	0xa5, 0x2b, 0x3e, 0xba, 0x8f, 0x82, 0xa8, 0x71, 0x80, 0xa9, 0xf0, 0x50,
	0x00, 0x86, 0x2a, 0x42, 0xa1, 0xcb, 0x1b, 0xa3, 0xa8, 0xfb, 0x87, 0xaa,
	0xfb, 0x9f, 0xa9, 0xf0, 0x96, 0x2e, 0x01, 0x00,
};
static const u8 ring_mpi[] = {	/* RING_ZL880_BB90V_DEF: ringing generator */
	0xc0, 0x08, 0x00, 0x00, 0x00, 0x44, 0x3a, 0x9d, 0x00, 0x00, 0x00, 0x00,
};

/*
 * SLIC channel selector for the FXS port. The Le9642 is a 2-channel device:
 * VP886_EC_1 (0x01) = channel 1 (the working line; case-labeled "phone2" --
 * the case labels are inverted vs the silicon, same as the ethernet ports),
 * VP886_EC_2 (0x02) = channel 2 (2nd jack, case-labeled "phone1"). Probe: set
 * slic_ec=2 to move the single line to the 2nd FXS channel, reusing the same
 * timeslot/DMA/char device/baresip. Device-level init + chip detect stay on
 * EC_1; only the per-channel line config, ring and hook follow this. */
static int slic_ec = VP886_EC_1;
module_param(slic_ec, int, 0644);

/* PCM bus timeslot for this channel's voice (TXSLOT mic->bus, RXSLOT bus->ear).
 * ch1 uses slot 4 (default). For two simultaneous lines, ch2 must use its OWN
 * slot (e.g. 6) so the two lines don't collide on the bus. The SoC PCM RX DMA
 * channel that the mic lands on shifts with this slot (find via rx_scan). */
static int slic_slot = 4;
module_param(slic_slot, int, 0644);

/* Stream one raw MPI section to the SLIC in a single ZSI session. */
static int slic_write_mpi(const char *what, const u8 *b, int n)
{
	int i, ret;

	sd.where = what;
	zsi_begin();
	for (i = 0; i < n; i++) {
		ret = zsi_write_byte(b[i]);
		if (ret)
			return ret;
	}
	return 0;
}

/* Load device + DC + AC + ring profiles (Raw MPI sections). */
static int slic_load_profiles(void)
{
	int ret;

	ret = slic_write_mpi("dev", dev_mpi, sizeof(dev_mpi));
	if (ret)
		return ret;
	ret = slic_write_mpi("dc", dc_mpi, sizeof(dc_mpi));
	if (ret)
		return ret;
	ret = slic_write_mpi("ac", ac_mpi + 6, sizeof(ac_mpi) - 6);
	if (ret)
		return ret;
	return slic_write_mpi("ring", ring_mpi, sizeof(ring_mpi));
}

/*
 * Load the per-channel line profiles (DC feed, AC impedance, ring generator)
 * onto the active SLIC channel (slic_ec). slic_load_profiles()/init only
 * program channel 1; the 2nd FXS channel needs its own copy for a matched
 * hybrid + ring. No-op on channel 1 (already loaded). Mirrors the DC/AC/ring
 * sections of slic_load_profiles, just on the selected channel.
 */
static void slic_load_channel_profiles(void)
{
	if (slic_ec == VP886_EC_1)
		return;
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	slic_write_mpi("dc-ch", dc_mpi, sizeof(dc_mpi));
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	slic_write_mpi("ac-ch", ac_mpi + 6, sizeof(ac_mpi) - 6);
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	slic_write_mpi("ring-ch", ring_mpi, sizeof(ring_mpi));
}

/* Load DC/AC/ring profiles onto an EXPLICIT channel (EC_1 or EC_2). Same bytes
 * as slic_load_channel_profiles but for an arbitrary ec -- needed to ring both
 * jacks (EC_1's profiles otherwise come only implicitly from slic_load_profiles
 * landing on the post-reset default channel). */
static void slic_load_dc_ac_ring(u8 ec)
{
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ ec }, 1);
	slic_write_mpi("dc-ch", dc_mpi, sizeof(dc_mpi));
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ ec }, 1);
	slic_write_mpi("ac-ch", ac_mpi + 6, sizeof(ac_mpi) - 6);
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ ec }, 1);
	slic_write_mpi("ring-ch", ring_mpi, sizeof(ring_mpi));
}

/* Assert the ring state on ONE channel. on=1 -> ring (STATE 0x07). on=0 -> stop
 * + feed (STATE 0x23 active+codec) so the hook stays detectable in the OFF gap.
 * Profiles for ec must already be loaded. Byte-for-byte the same sequence the
 * single-channel path used, just parameterised by ec. */
static void slic_ring_state(u8 ec, bool on)
{
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ ec }, 1);
	if (on) {
		{ static const u8 sw[] = { 0xe6, 0x6f }; slic_write_mpi("ring-sw", sw, sizeof(sw)); }
		zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ ec }, 1);
		{ static const u8 st[] = { 0x56, 0x03 }; slic_write_mpi("ring-active", st, sizeof(st)); }
		msleep(20);
		zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ ec }, 1);
		{ static const u8 r[] = { 0x56, 0x07 }; slic_write_mpi("ring-on", r, sizeof(r)); }
	} else {
		static const u8 st[] = { 0x56, 0x23 }; slic_write_mpi("ring-off", st, sizeof(st));
	}
}

/* Full 3b init: bring-up (3a) + verify + load device profile. */
static int slic_init(u8 id[2], u8 *devmode_out)
{
	int ret;

	mutex_lock(&sd.lock);
	zsi_hw_init();
	zsi_slic_reset();
	writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);

	ret = zsi_mpi_read(VP886_EC_1, VP886_R_RCNPCN_RD, id, 2);
	if (ret)
		goto out;
	ret = slic_load_profiles();
	if (ret)
		goto out;
	/* read the Device Mode register back to confirm the profile took */
	ret = zsi_mpi_read(VP886_EC_1, 0x5f, devmode_out, 1);
out:
	mutex_unlock(&sd.lock);
	return ret;
}

extern int pcm_en751221_capture_rx(u8 *out, int nbytes);

/*
 * SLIC line up for voice. Values are the OEM ground truth (le9641.c) + what
 * Codex proved live to make the earpiece audible:
 *   - switcher HP (0x6f), feed active
 *   - TX slot 4, RX slot 4: OEM uses timeSlotIdx 2 << 1 = 4 for 16-bit LINEAR
 *     (16-bit = 2 8-bit slots). With slot 6 the PCM TX never reaches the bus
 *     (TSA loopback all-zero); slot 4 lands the audio on DMA ch0.
 *   - OPFUNC codec = LINEAR (0x80), NOT u-law -- OEM logs "CODEC_LINEAR".
 *   - GR (receive/earpiece gain, MPI 0x82) = 0x4000 unity; without it the
 *     earpiece RX path is muted.
 */
/*
 * Capture-codec tuning (live, no rebuild). The SADC probe proved the mic AC
 * signal IS on the line; the codec capture just isn't grabbing it unless the
 * DAC is active (codec-gate). These let us sweep the OEM-correct activation.
 *   icr4_mode: 0 = VADC+VDAC fully automatic (ICR4=0); 1 = force VDAC only,
 *              VADC automatic (OEM-like; forcing VADC may pin the ADC marginal);
 *              2 = force both (the old known-idle-issue config).
 *   settle_ms: delay after STATE active+codec for the ADC front-end to converge.
 *   hpf_on:    1 = also clear OPCOND HIGHPASS_DIS (HPF on, OEM, kills DC offset).
 */
static int icr4_mode = 2;	/* validated default (VADC+VDAC forced) */
module_param(icr4_mode, int, 0644);
static int settle_ms = 20;
module_param(settle_ms, int, 0644);
static int hpf_on = 1;
module_param(hpf_on, int, 0644);
static int gx_val;	/* GX capture gain register (0x80) override; 0 = profile */
module_param(gx_val, int, 0644);
/* DC feed loop-current limit ILA (DCFEED 0xc6 byte1 bits0-4). mA = 18 + field.
 * The cordless DECT base needs more feed current than the 600R AC profile
 * default (0x07 = 25mA) for the mic to reach the VADC; 0x14 = 38mA captured
 * clean (no clip) in testing. 0 = leave the profile value. Live-tunable. */
static int feed_ila = 0x14;
module_param(feed_ila, int, 0644);

static int slic_audio_setup(void)
{
	u8 v;
	int ret;

	/* For the 2nd FXS channel, give it its own DC/AC/ring line profiles first
	 * (init only programmed channel 1). No-op on channel 1. */
	slic_load_channel_profiles();

	/*
	 * Switcher timing + params (from OEM profile) BEFORE enabling the switcher,
	 * so the buck-boost runs with calibrated edge timing instead of noisy
	 * defaults (uncalibrated ripple couples broadband noise into the ADC).
	 *
	 * KNOWN ISSUE -- idle "tssss" DC-DC coil whine (TODO, not yet fixed here):
	 * the buzz is light-load burst/PFM noise from holding the line in active
	 * feed (STATE 0x23) at on-hook idle; it is loud unloaded (on-hook) and
	 * vanishes under load (off-hook). It is NOT fixable from these three
	 * switcher registers alone -- verified live on the Le9642 (an ABS part,
	 * PCN 0x75): swctrl HP/MED/LP (0x6f/0x6a/0x65), the OEM buck-boost timing
	 * word (0xf6 95 00 58 30 5c 30) and AUTO swparam variants (0x04/0x44) all
	 * still buzz at idle; swparam POWER_MODE_MANUAL(0x80)/COUNTER(0x20) DO
	 * silence it but only by starving the rail, which drops the off-hook loop
	 * current below the SIGREG(0x4d) hook threshold -> live calls drop. The
	 * real OEM fix is to put the line in a low-power STANDBY state at idle
	 * (STATE SS_LOWPOWER 0x0c) with the LPM metallic-hook comparator armed
	 * (LOOPSUP 0xc2) and a standby<->active state machine -- substantial work,
	 * deferred. For now we keep the proven HP/AUTO active-feed config: calls
	 * work, idle buzz remains (gone during calls). Live A/B knobs:
	 * swtiming / swparam / swctrl / state debugfs nodes.
	 */
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ static const u8 swt[] = { 0xf6, 0x66, 0x00, 0x64, 0x30, 0x74, 0x30 }; slic_write_mpi("swtiming", swt, sizeof(swt)); }
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ static const u8 swp[] = { 0xe4, 0x04, 0x92, 0x0a }; slic_write_mpi("swparam", swp, sizeof(swp)); }
	/* switcher HP (proven config; off-hook loop current stays above the hook threshold) */
	{ static const u8 sw[] = { 0xe6, 0x6f }; ret = slic_write_mpi("sw-hp", sw, sizeof(sw)); if (ret) return ret; }
	/* feed active (no codec yet) */
	ret = zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	if (ret) return ret;
	{ static const u8 st[] = { 0x56, 0x03 }; slic_write_mpi("active", st, sizeof(st)); }

	/* Raise the DC feed loop-current limit (ILA, DCFEED 0xc6 byte1 bits0-4).
	 * The cordless DECT base needs more feed current than the 600R AC profile
	 * default (ILA=0x07=25mA) for the mic modulation to reach the VADC --
	 * otherwise the capture stays idle "00 XX" unless the DAC stirs the line.
	 * Preserve byte0 (VOC) and byte1 bits5-7 (LONG_IMPED). feed_ila=0 keeps the
	 * profile value. */
	if (feed_ila != 0) {
		u8 dcf[2];

		if (!zsi_mpi_read((u8)slic_ec, 0xc7, dcf, sizeof(dcf))) {
			dcf[1] = (dcf[1] & ~0x1f) | (feed_ila & 0x1f);
			zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
			{ u8 d[3] = { 0xc6, dcf[0], dcf[1] }; slic_write_mpi("dcfeed-ila", d, 3); }
		}
	}

	/* u-law (G.711) codec, all AC filters active. The Le9642 drives only 8 bits
	 * per timeslot even in "linear" mode (the captured LSB/MSB byte is the only
	 * data, the other is 0), so 16-bit linear gives 8-bit resolution -> quiet
	 * speech quantizes to 0 -> choppy. u-law companding (8-bit log) preserves
	 * quiet speech; the voice thread (de)compands to/from 16-bit linear for the
	 * char device. Written BEFORE the timeslots (OEM order, le9641.c). */
	v = 0x7f;	/* OPFUNC = CODEC_ULAW (0x40) | ALL_FILTERS (0x3f) */
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ u8 of[2] = { 0x60, v }; slic_write_mpi("opfunc", of, 2); }

	/* TX slot (mic -> bus), RX slot (earpiece <- bus). slic_slot selects the
	 * bus timeslot so a 2nd channel can use its own slot (default 4 = ch1). */
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ u8 tx[] = { 0x40, (u8)slic_slot }; slic_write_mpi("txslot", tx, sizeof(tx)); }
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ u8 rx[] = { 0x42, (u8)slic_slot }; slic_write_mpi("rxslot", rx, sizeof(rx)); }

	/* ICR3/ICR4 are mask,data pairs: set mask bit and data bit. */
	{
		u8 icr3[4], icr4[4];

		ret = zsi_mpi_read((u8)slic_ec, 0xf3, icr3, sizeof(icr3));
		if (ret) return ret;
		icr3[0] |= 0x01;	/* VREF_EN mask */
		icr3[1] |= 0x01;	/* VREF_EN data */
		zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
		ret = slic_write_mpi("icr3-vref", (const u8[]){ 0xf2, icr3[0], icr3[1], icr3[2], icr3[3] }, 5);
		if (ret) return ret;

		/* ICR4 [0]=override mask, [1]=data; [2]=mask, [3]=data. Bits:
		 * VADC_EN 0x01, VDAC_EN 0x02, AISN_CTRL 0x10, SMALL_SIGNAL_CTRL 0x80.
		 * The OEM/VP-API init writes ICR4 = {0,0,0,0} so the HW state machine
		 * auto-enables VADC/VDAC/AISN/small-signal in active+codec. A nonzero
		 * MASK bit with value 0 FORCE-DISABLES that block -> "ADC senses
		 * nothing until the DAC stirs the line" (our exact bug). So write
		 * ICR4 from ZERO (no read-modify-write that could preserve a stray
		 * AISN/small-signal mask bit). icr4_mode 0 = fully automatic (OEM). */
		icr4[0] = icr4[1] = icr4[2] = icr4[3] = 0;
		if (icr4_mode == 1) {		/* force VDAC only (DAC), rest auto */
			icr4[0] = 0x02;
			icr4[1] = 0x02;
		} else if (icr4_mode == 2) {	/* force VADC+VDAC (old config) */
			icr4[0] = 0x03;
			icr4[1] = 0x03;
		}
		zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
		ret = slic_write_mpi("icr4", (const u8[]){ 0xf4, icr4[0], icr4[1], icr4[2], icr4[3] }, 5);
		if (ret) return ret;
	}

	/* GR receive (earpiece) gain = 0x4000 unity (MPI write opcode 0x82) */
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ static const u8 gr[] = { 0x82, 0x40, 0x00 }; slic_write_mpi("gr", gr, sizeof(gr)); }

	/* GX transmit (capture/mic) gain (opcode 0x80, 2 bytes). The mic is
	 * captured at very low amplitude; the AC profile sets GX=0xA9F0. gx_val
	 * lets us boost it in the analog codec (before its noise) -- 0 = leave the
	 * profile value. Tune live: echo VAL > .../parameters/gx_val (decimal). */
	if (gx_val != 0) {
		zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
		{ u8 gx[3] = { 0x80, (gx_val >> 8) & 0xff, gx_val & 0xff };
		  slic_write_mpi("gx", gx, sizeof(gx)); }
	}

	/* OPCOND: clear CUT_TX|CUT_RX|TSA_LOOPBACK (and HIGHPASS_DIS if hpf_on,
	 * so the codec high-pass is active like the OEM -> removes DC offset). */
	ret = zsi_mpi_read((u8)slic_ec, 0x71, &v, 1);
	if (ret) return ret;
	v &= hpf_on ? ~0xe4 : ~0xc4;
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ u8 oc[2] = { 0x70, v }; slic_write_mpi("opcond", oc, 2); }

	/* STATE = active + codec (0x23) */
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ static const u8 st[] = { 0x56, 0x23 }; slic_write_mpi("active+codec", st, sizeof(st)); }

	/* let the ADC front-end bias/settle before the first capture */
	if (settle_ms > 0)
		msleep(settle_ms);

	/* Enable ADC dither-cancel correction (INDCAL byte1 bit 0x80) to drop the
	 * codec capture noise floor. Read INDCAL (0xb1) first to preserve byte0
	 * (HD2 fuse trim). */
	{
		u8 ind[3];

		if (!zsi_mpi_read((u8)slic_ec, 0xb1, ind, sizeof(ind))) {
			ind[1] |= 0x80;
			zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
			{ u8 ic[4] = { 0xb0, ind[0], ind[1], ind[2] }; slic_write_mpi("indcal-dither", ic, 4); }
		}
	}
	return 0;
}

static int slic_audio_show(struct seq_file *s, void *unused)
{
	u8 buf[80];
	u8 txslot = 0xff, opfunc = 0xff, st = 0xff;
	int ret, i, mn = 255, mx = 0, nonzero = 0;

	mutex_lock(&sd.lock);
	zsi_hw_init();
	zsi_slic_reset();
	writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
	/* load profiles, then audio line-up */
	slic_load_profiles();
	ret = slic_audio_setup();
	zsi_mpi_read((u8)slic_ec, 0x41, &txslot, 1);
	zsi_mpi_read((u8)slic_ec, 0x61, &opfunc, 1);
	zsi_mpi_read((u8)slic_ec, 0x57, &st, 1);
	mutex_unlock(&sd.lock);

	memset(buf, 0, sizeof(buf));
	if (!ret)
		ret = pcm_en751221_capture_rx(buf, sizeof(buf));

	for (i = 0; i < (int)sizeof(buf); i++) {
		if (buf[i] < mn) mn = buf[i];
		if (buf[i] > mx) mx = buf[i];
		if (buf[i] != buf[0]) nonzero++;
	}
	seq_printf(s, "slic: txslot=0x%02x opfunc=0x%02x state=0x%02x  capture ret=%d\n",
		   txslot, opfunc, st, ret);
	seq_printf(s, "rx samples min=0x%02x max=0x%02x spread=%d varying=%d/80\n",
		   mn, mx, mx - mn, nonzero);
	seq_printf(s, "first16: %16ph\n", buf);
	if (mx - mn > 4)
		seq_puts(s, "AUDIO present (samples vary -- mic signal captured)\n");
	else
		seq_puts(s, "no audio variation (silence / phone on-hook / wrong slot)\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_audio);

extern int pcm_en751221_capture_allch(u8 *out);
extern int pcm_en751221_play_melody(void);
extern int pcm_en751221_play_tone_1khz(void);
extern int pcm_en751221_voice_loopback(int seconds);
extern int pcm_en751221_voice_start(void);
extern void pcm_en751221_voice_reset(void);
extern void pcm_en751221_voice_set_mute(int on);
/* fwd: persistent line-up flag (defined once, set by audio_setup/voice open) */
static bool slic_audio_up;
extern void pcm_en751221_voice_stop(void);
extern ssize_t pcm_en751221_voice_read(char __user *ubuf, size_t len);
extern ssize_t pcm_en751221_voice_write(const char __user *ubuf, size_t len);

/*
 * On-hook fast-mute. While a call is up (/dev/xr500v-voice open) a process-
 * context delayed_work polls the SLIC hook; the moment the line goes on-hook it
 * mutes the captured TX audio (PCM silence flag) so the loud dead-line transient
 * is not streamed to the peer for the ~450ms the callmgr takes to debounce +
 * hang up. It only mutes audio, never call control, so a transient false on-hook
 * just mutes for <=one poll and self-heals. Runs only during INCALL (no ring to
 * corrupt the hook bit). ZSI I/O sleeps, so this MUST be process context, never
 * a timer (a timer doing ZSI panics 'scheduling while atomic').
 */
static struct delayed_work hook_poll_work;
static bool hook_poll_running;
static int hook_poll_ms = 40;	/* a 4-byte ZSI read holds sd.lock ~8-16ms at zsi_gap_us=1000 */
module_param(hook_poll_ms, int, 0644);

static void hook_poll_fn(struct work_struct *w)
{
	u8 sig[4] = {0};
	int idx = (slic_ec >= 1 && slic_ec <= 4) ? (slic_ec - 1) : 0;
	int rc;

	mutex_lock(&sd.lock);
	rc = zsi_mpi_read((u8)slic_ec, 0x4d, sig, sizeof(sig));
	mutex_unlock(&sd.lock);

	/* keep the last mute state on a ZSI read error -- never mute on a glitch */
	if (rc == 0)
		pcm_en751221_voice_set_mute(!(sig[idx] & 0x01));

	if (READ_ONCE(hook_poll_running))
		schedule_delayed_work(&hook_poll_work, msecs_to_jiffies(hook_poll_ms));
}

/*
 * /dev/xr500v-voice -- full-duplex 16-bit linear 8kHz PCM voice device.
 * open: line the SLIC up for voice + start continuous DMA streaming.
 * read = mic, write = earpiece. release: stop streaming.
 */
static int voice_dev_open(struct inode *ino, struct file *f)
{
	int ret;

	/*
	 * Full-duplex stream: read (mic) and write (earpiece) come from two
	 * separate threads sharing this one fd (e.g. baresip's ausrc + auplay).
	 * Mark it stream-mode so the VFS does NOT take the per-file f_pos lock
	 * on read/write -- otherwise a writer blocked in voice_write() holds
	 * f_pos_lock and starves the reader (capture throttled to ~3% -> TX=0).
	 */
	stream_open(ino, f);

	/*
	 * Rewind the PCM DMA engine BEFORE the SLIC line-up. Without this the
	 * engine keeps the descriptor pointer from the previous call and the 2nd+
	 * call desyncs against the voice thread -> ring starves -> earpiece chops
	 * (ISR reg 0x24 = 0x3d vs a healthy 0x0d). It must precede the line-up
	 * because the soft reset blips the TDM clock the SLIC locks onto.
	 */
	pcm_en751221_voice_reset();

	mutex_lock(&sd.lock);
	zsi_hw_init();
	zsi_slic_reset();
	writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
	slic_load_profiles();
	slic_audio_up = (slic_audio_setup() == 0);
	mutex_unlock(&sd.lock);
	if (!slic_audio_up)
		return -EIO;
	ret = pcm_en751221_voice_start();
	if (ret)
		return ret;
	WRITE_ONCE(hook_poll_running, true);
	schedule_delayed_work(&hook_poll_work, msecs_to_jiffies(hook_poll_ms));
	return 0;
}

static int voice_dev_release(struct inode *ino, struct file *f)
{
	/* stop the hook poller (not holding sd.lock here, so the sync cancel that
	 * waits for hook_poll_fn -- which takes sd.lock -- cannot deadlock) */
	WRITE_ONCE(hook_poll_running, false);
	cancel_delayed_work_sync(&hook_poll_work);
	pcm_en751221_voice_set_mute(0);
	pcm_en751221_voice_stop();
	return 0;
}

static ssize_t voice_dev_read(struct file *f, char __user *buf, size_t len, loff_t *o)
{
	return pcm_en751221_voice_read(buf, len);
}

static ssize_t voice_dev_write(struct file *f, const char __user *buf, size_t len, loff_t *o)
{
	return pcm_en751221_voice_write(buf, len);
}

static const struct file_operations voice_fops = {
	.owner		= THIS_MODULE,
	.open		= voice_dev_open,
	.release	= voice_dev_release,
	.read		= voice_dev_read,
	.write		= voice_dev_write,
};

static struct miscdevice voice_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "xr500v-voice",
	.fops	= &voice_fops,
};
extern int pcm_en751221_loopback_capture(u8 *out, u32 intface);
extern int pcm_en751221_loopback_tone_capture(u8 *out, int nbytes);

static int slic_audio_setup_show(struct seq_file *s, void *unused)
{
	int ret;
	u8 tx = 0xff, st = 0xff, sig = 0xff;

	mutex_lock(&sd.lock);
	zsi_hw_init();
	zsi_slic_reset();
	writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
	slic_load_profiles();
	ret = slic_audio_setup();
	zsi_mpi_read((u8)slic_ec, 0x41, &tx, 1);
	zsi_mpi_read((u8)slic_ec, 0x57, &st, 1);
	zsi_mpi_read((u8)slic_ec, 0x4d, &sig, 1);
	mutex_unlock(&sd.lock);
	slic_audio_up = (ret == 0);
	seq_printf(s, "audio setup ret=%d  txslot=0x%02x state=0x%02x hook=%d\n",
		   ret, tx, st, sig & 1);
	seq_puts(s, slic_audio_up ? "line UP -- now: cat rx_scan (with noise)\n"
				  : "setup FAILED\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_audio_setup);

/* Scan all 8 RX channels -- show which one carries the SLIC mic audio. */
static int rx_scan_show(struct seq_file *s, void *unused)
{
	static u8 buf[8 * 80];
	int ret, ch, i, mn, mx;

	ret = pcm_en751221_capture_allch(buf);
	seq_printf(s, "capture_allch ret=%d\n", ret);
	for (ch = 0; ch < 8; ch++) {
		u8 *b = buf + ch * 80;

		mn = 255; mx = 0;
		for (i = 0; i < 80; i++) {
			if (b[i] < mn) mn = b[i];
			if (b[i] > mx) mx = b[i];
		}
		seq_printf(s, " ch%d slot? min=0x%02x max=0x%02x spread=%3d %s  %8ph\n",
			   ch, mn, mx, mx - mn,
			   (mx - mn > 4) ? "<== SIGNAL" : "         ", b);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rx_scan);

/*
 * Write knob: set SWCTRL (0xe6) live to test switcher power modes without
 * re-running audio_setup. `echo 6a > swctrl` (MP/MP), 65 (LP/LP), 6f (HP/HP).
 * Gap timing is handled inside slic_write_mpi -- no fragile shell sleeps.
 */
static ssize_t swctrl_write(struct file *f, const char __user *ub,
			    size_t n, loff_t *off)
{
	u8 val;
	int ret;

	ret = kstrtou8_from_user(ub, n, 16, &val);
	if (ret)
		return ret;

	mutex_lock(&sd.lock);
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ u8 sw[2] = { 0xe6, val }; ret = slic_write_mpi("swctrl-dbg", sw, 2); }
	mutex_unlock(&sd.lock);

	pr_info("econet-slic: swctrl <- 0x%02x (ret=%d)\n", val, ret);
	return ret ? ret : n;
}
static const struct file_operations swctrl_fops = {
	.owner	= THIS_MODULE,
	.write	= swctrl_write,
	.open	= simple_open,
	.llseek	= default_llseek,
};

/*
 * Write the System State register (0x56) directly, for experimenting with the
 * line-feed states live: `echo 04 > state` (SS_IDLE, low-power standby),
 * `echo 0c > state` (SS_LOWPOWER), `echo 23 > state` (active+codec, the
 * in-call default), `echo 03 > state` (active). The buzz on the FXS line is
 * the buck-boost DC-DC sustaining the active feed (0x23) -- a true low-power
 * idle state should silence it while keeping SIGREG hook detection.
 */
static ssize_t state_write(struct file *f, const char __user *ub,
			   size_t n, loff_t *off)
{
	u8 val;
	int ret;

	ret = kstrtou8_from_user(ub, n, 16, &val);
	if (ret)
		return ret;

	mutex_lock(&sd.lock);
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ u8 st[2] = { 0x56, val }; ret = slic_write_mpi("state-dbg", st, 2); }
	mutex_unlock(&sd.lock);

	pr_info("econet-slic: state(0x56) <- 0x%02x (ret=%d)\n", val, ret);
	return ret ? ret : n;
}
static const struct file_operations state_fops = {
	.owner	= THIS_MODULE,
	.write	= state_write,
	.open	= simple_open,
	.llseek	= default_llseek,
};

/*
 * Write SWPARAM[0] (Switching Regulator Parameters byte0, reg 0xE4), keeping
 * bytes 1-2 at the OEM defaults (0x92 0x0a). byte0 bit7 = POWER_MODE
 * (0=AUTO/tracker, 0x80=MANUAL/fixed); low 5 bits = floor/ABS voltage.
 * Hypothesis for the idle "coil whine" buzz: in AUTO the tracker collapses the
 * rail at light load (on-hook) so the buck-boost runs in audible burst mode;
 * MANUAL (0x80|floor) keeps a fixed rail -> continuous (inaudible) switching.
 * e.g. `echo 84 > swparam` = MANUAL + floor 4 (~25V).
 */
static ssize_t swparam_write(struct file *f, const char __user *ub,
			     size_t n, loff_t *off)
{
	u8 val;
	int ret;

	ret = kstrtou8_from_user(ub, n, 16, &val);
	if (ret)
		return ret;

	mutex_lock(&sd.lock);
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ u8 sp[4] = { 0xe4, val, 0x92, 0x0a }; ret = slic_write_mpi("swparam-dbg", sp, 4); }
	mutex_unlock(&sd.lock);

	pr_info("econet-slic: swparam[0](0xe4) <- 0x%02x (ret=%d)\n", val, ret);
	return ret ? ret : n;
}
static const struct file_operations swparam_fops = {
	.owner	= THIS_MODULE,
	.write	= swparam_write,
	.open	= simple_open,
	.llseek	= default_llseek,
};

/*
 * Write the 6-byte SWTIMING register (0xF6, buck-boost on/off-time word) live,
 * for A/B-testing the switcher timing without a rebuild:
 *   echo "95 00 58 30 5c 30" > swtiming   (OEM Le9642 buck-boost word)
 * The driver's old default 66 00 64 30 74 30 was a Le9661 flyback word and was
 * the wrong-topology timing that made the converter burst (whine) at light load.
 */
static ssize_t swtiming_write(struct file *f, const char __user *ub,
			      size_t n, loff_t *off)
{
	char buf[64];
	u8 t[7] = { 0xf6 };
	int ret;

	if (n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ub, n))
		return -EFAULT;
	buf[n] = '\0';
	if (sscanf(buf, "%hhx %hhx %hhx %hhx %hhx %hhx",
		   &t[1], &t[2], &t[3], &t[4], &t[5], &t[6]) != 6)
		return -EINVAL;

	mutex_lock(&sd.lock);
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	ret = slic_write_mpi("swtiming-dbg", t, sizeof(t));
	mutex_unlock(&sd.lock);

	pr_info("econet-slic: swtiming(0xf6) <- %02x %02x %02x %02x %02x %02x (ret=%d)\n",
		t[1], t[2], t[3], t[4], t[5], t[6], ret);
	return ret ? ret : n;
}
static const struct file_operations swtiming_fops = {
	.owner	= THIS_MODULE,
	.write	= swtiming_write,
	.open	= simple_open,
	.llseek	= default_llseek,
};

/*
 * Generic MPI register write knob (any register), for experimenting with
 * loop-supervision / low-power-standby register sequences live without a
 * rebuild. Format: space-separated hex bytes, first byte = register address,
 * rest = data. e.g. `echo "c2 60 00" > mpi` writes reg 0xC2 = 60 00.
 * Up to 15 data bytes. Use with care -- arbitrary register writes.
 */
static ssize_t mpi_write(struct file *f, const char __user *ub,
			 size_t n, loff_t *off)
{
	char buf[96];
	u8 b[16];
	int i = 0, ret, pos = 0, consumed = 0;
	unsigned int v;

	if (n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ub, n))
		return -EFAULT;
	buf[n] = '\0';
	while (i < (int)sizeof(b) && sscanf(buf + pos, " %x%n", &v, &consumed) == 1) {
		b[i++] = (u8)v;
		pos += consumed;
	}
	if (i < 1)
		return -EINVAL;

	mutex_lock(&sd.lock);
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	ret = slic_write_mpi("mpi-dbg", b, i);
	mutex_unlock(&sd.lock);

	pr_info("econet-slic: mpi reg 0x%02x <- %d byte(s) (ret=%d)\n", b[0], i - 1, ret);
	return ret ? ret : n;
}
static const struct file_operations mpi_fops = {
	.owner	= THIS_MODULE,
	.write	= mpi_write,
	.open	= simple_open,
	.llseek	= default_llseek,
};

/*
 * Generic MPI register READ knob: `echo "57 1" > mpi_rd` reads 1 byte of the
 * read-side register 0x57 (STATE readback) on the active channel; result goes
 * to dmesg. Format: "<reg> [<count>]" (count default 1, max 8). For verifying
 * the low-power-standby register sequence (STATE readback 0x57, LOOPSUP, etc).
 */
static ssize_t mpi_rd_write(struct file *f, const char __user *ub,
			    size_t n, loff_t *off)
{
	char buf[32];
	u8 out[8];
	unsigned int reg, cnt = 1;
	int ret, i;
	char hex[3 * sizeof(out) + 1];

	if (n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ub, n))
		return -EFAULT;
	buf[n] = '\0';
	if (sscanf(buf, "%x %u", &reg, &cnt) < 1)
		return -EINVAL;
	if (cnt < 1 || cnt > sizeof(out))
		cnt = 1;

	mutex_lock(&sd.lock);
	ret = zsi_mpi_read((u8)slic_ec, (u8)reg, out, (u8)cnt);
	mutex_unlock(&sd.lock);

	for (i = 0; i < (int)cnt; i++)
		snprintf(hex + i * 3, 4, "%02x ", out[i]);
	pr_info("econet-slic: mpi_rd reg 0x%02x [%u] = %s(ret=%d)\n", reg, cnt, hex, ret);
	return ret ? ret : n;
}
static const struct file_operations mpi_rd_fops = {
	.owner	= THIS_MODULE,
	.write	= mpi_rd_write,
	.open	= simple_open,
	.llseek	= default_llseek,
};

/*
 * Low-power standby control for the idle-line DC-DC coil-whine fix.
 *   echo 1 > lpm   -> enter SS_LOWPOWER standby (silent): arm the LPM metallic-
 *                     hook comparator (LOOPSUP 0xc2) + disable the chip's
 *                     auto-LPM-exit (SSCFG 0x68 AUTO_SYSSTATE_LPM_DIS, so it
 *                     stays parked in low power instead of self-jumping back to
 *                     the active feed and whining), then STATE=0x0c + SWCTRL=LP.
 *   echo 0 > lpm   -> active feed for a call: SWCTRL=HP first, then STATE=0x23.
 * Off-hook is detected on the SAME hook node (SIGREG 0x4d bit0) in both states.
 * Operates on the currently-selected slic_ec channel. Verified live: silent
 * on-hook, hook=0 idle, hook=1 on lift, calls work. Driven by xr500v-callmgr:
 * standby at IDLE, active on off-hook / in-call.
 */
static void slic_lpm_arm_locked(void)
{
	u8 ss[3] = { 0x68, 0, 0 };
	/* LOOPSUP: byte3=0x8e LPM_HOOK_THRESH(~22V) + byte4=0x03 HOOK_AVG_EN|HSH_CTL_DBNC
	 * -> arms the SS_LOWPOWER voltage hook comparator (API-init value). */
	{ static const u8 ls[] = { 0xc2, 0x5b, 0x84, 0xb3, 0x8e, 0x03 };
	  slic_write_mpi("lpm-loopsup", ls, sizeof(ls)); }
	/* SSCFG read-modify-write: set AUTO_SYSSTATE_LPM_DIS (byte1 bit0x02) so the
	 * silicon will not auto-exit LPM back to the (whining) active feed. */
	if (zsi_mpi_read((u8)slic_ec, 0x69, &ss[1], 2) == 0) {
		ss[2] |= 0x02;
		slic_write_mpi("lpm-sscfg", ss, sizeof(ss));
	}
}

static ssize_t lpm_write(struct file *f, const char __user *ub,
			 size_t n, loff_t *off)
{
	u8 val;
	int ret;

	ret = kstrtou8_from_user(ub, n, 10, &val);
	if (ret)
		return ret;

	mutex_lock(&sd.lock);
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	if (val) {
		slic_lpm_arm_locked();
		{ static const u8 st[] = { 0x56, 0x0c }; slic_write_mpi("lpm-standby", st, 2); }
		mutex_unlock(&sd.lock);
		msleep(20);	/* defer the power-down so the rail isn't cut mid-transition */
		mutex_lock(&sd.lock);
		zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
		{ static const u8 sw[] = { 0xe6, 0x65 }; slic_write_mpi("lpm-sw-lp", sw, 2); }
	} else {
		/* up: switcher HP FIRST, then active feed (rail up before line on) */
		{ static const u8 sw[] = { 0xe6, 0x6f }; slic_write_mpi("lpm-sw-hp", sw, 2); }
		{ static const u8 st[] = { 0x56, 0x23 }; slic_write_mpi("lpm-active", st, 2); }
	}
	mutex_unlock(&sd.lock);
	pr_info("econet-slic: lpm <- %s\n", val ? "standby" : "active");
	return n;
}
static const struct file_operations lpm_fops = {
	.owner	= THIS_MODULE,
	.write	= lpm_write,
	.open	= simple_open,
	.llseek	= default_llseek,
};

/*
 * Ring control for the "real call" flow: `echo 1 > ring` makes the FXS line
 * ring (so a DECT handset treats it as a real incoming call and engages its
 * mic when answered); `echo 0 > ring` drops to active+codec (= answered).
 * Done over ZSI without opening the char device, so a userspace agent can ring
 * on a SIP INVITE and poll `hook` for off-hook before telling baresip to answer.
 */
/* Ring BOTH FXS jacks (EC_1=phone2, EC_2=phone1) on an incoming call so the one
 * phone rings in whichever jack it is plugged into. 0 = legacy single-channel
 * (slic_ec) -- byte-identical to the pre-ring-both behaviour. */
static int ring_both = 1;
module_param(ring_both, int, 0644);

static ssize_t ring_write(struct file *f, const char __user *ub,
			  size_t n, loff_t *off)
{
	u8 val;
	int ret = kstrtou8_from_user(ub, n, 10, &val);

	if (ret)
		return ret;

	mutex_lock(&sd.lock);
	if (val) {
		/* device-level bring-up once: resets the chip + loads EC_1's
		 * (post-reset default channel) profiles */
		zsi_hw_init();
		zsi_slic_reset();
		writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
		slic_load_profiles();
		if (ring_both) {
			slic_load_dc_ac_ring(VP886_EC_2);	/* EC_1 from load_profiles, EC_2 explicit */
			slic_ring_state(VP886_EC_1, true);
			slic_ring_state(VP886_EC_2, true);	/* EC_2 last: tail identical to phone1-only */
		} else {
			slic_load_channel_profiles();		/* legacy: slic_ec channel */
			slic_ring_state((u8)slic_ec, true);
		}
	} else {
		if (ring_both) {
			slic_ring_state(VP886_EC_1, false);
			slic_ring_state(VP886_EC_2, false);
		} else {
			slic_ring_state((u8)slic_ec, false);
		}
	}
	mutex_unlock(&sd.lock);
	return n;
}
static const struct file_operations ring_fops = {
	.owner	= THIS_MODULE,
	.write	= ring_write,
	.open	= simple_open,
	.llseek	= default_llseek,
};

/* Read the SLIC hook state (SIGREG bit0): 1 = off-hook (handset lifted). */
/*
 * Read the SLIC hook state (off-hook = handset lifted). SIGREG (0x4d) is a
 * DEVICE-level 4-byte register: each channel's HOOK bit (0x01) lives in its
 * OWN byte -- byte[0]=channel 1 (EC_1), byte[1]=channel 2 (EC_2). So index by
 * (slic_ec - 1), not a fixed byte[0]. (Empirically confirmed: off-hook on ch2
 * reads 00 01 00 00.) Read with the no-update-latch addr so polling doesn't
 * disturb the interrupt latch.
 */
static int slic_hook_show(struct seq_file *s, void *unused)
{
	u8 sig[4] = {0};
	int idx = (slic_ec >= 1 && slic_ec <= 4) ? (slic_ec - 1) : 0;

	mutex_lock(&sd.lock);
	zsi_mpi_read((u8)slic_ec, 0x4d, sig, sizeof(sig));
	mutex_unlock(&sd.lock);
	seq_printf(s, "%d\n", sig[idx] & 1);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_hook);

/*
 * hook_raw: BOTH channels' hook bits from one device-level SIGREG (0x4d) read.
 * b0 = byte[0] = SLIC ch1 (EC_1) = physical jack "phone2",
 * b1 = byte[1] = SLIC ch2 (EC_2) = physical jack "phone1".
 * Lets the callmgr ring both jacks and learn which one was lifted. The
 * single-digit `hook` node above is untouched (it indexes the active slic_ec).
 */
static int slic_hook_raw_show(struct seq_file *s, void *unused)
{
	u8 sig[4] = {0};

	mutex_lock(&sd.lock);
	zsi_mpi_read((u8)slic_ec, 0x4d, sig, sizeof(sig));	/* SIGREG is device-level; ec arg is don't-care */
	mutex_unlock(&sd.lock);
	seq_printf(s, "%d %d\n", sig[0] & 1, sig[1] & 1);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_hook_raw);

/*
 * SADC probe: read the supervisory ADC (0xA0) -- a measurement path totally
 * independent of the voice codec (VADC->PCM). SEL TIP_RING_AC_V (0x00) reads
 * the AC voltage on tip/ring = the mic signal ON THE LINE; SEL METALLIC_CUR
 * (0x07) reads loop current (off-hook handset draws current). This tells us
 * whether the mic signal reaches the line at all (DECT transmitting) vs the
 * codec capture being idle (codec-gate). Run with the line up (cat
 * audio_setup), earpiece SILENT, while the user speaks into the handset.
 * One SADC math measurement: cfg via opcode 0xA0 (10 data bytes), wait, then
 * read B1 (0x75, 25 bytes) and parse like Vp886GetSingleAdcMath.
 */
static int sadc_one(u8 sel, int *spread, int *avg)
{
	u8 cfg[11] = { 0xa0, 0x50, sel, 0x00, 0x3c, 0x00, 0x0a, 0, 0, 0, 0 };
	u8 b1[25];
	int num, mn, mx, ret;
	long sum;

	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	ret = slic_write_mpi("sadc-cfg", cfg, sizeof(cfg));
	if (ret)
		return ret;
	msleep(60);
	ret = zsi_mpi_read((u8)slic_ec, 0x75, b1, sizeof(b1));
	if (ret)
		return ret;
	num = (b1[2] << 8) | b1[3];
	mn  = (s16)((b1[4] << 8) | b1[5]);
	mx  = (s16)((b1[6] << 8) | b1[7]);
	sum = (long)(int)((b1[8] << 24) | (b1[9] << 16) | (b1[10] << 8) | b1[11]);
	*spread = mx - mn;
	*avg = num ? (int)(sum / num) : 0;
	return num;
}

static int sadc_probe_show(struct seq_file *s, void *unused)
{
	int i, spread, avg, num, smin = 1 << 30, smax = 0, n = 0;
	long ssum = 0;

	mutex_lock(&sd.lock);
	seq_puts(s, "SADC tip/ring AC (mic on the line) -- speak into the handset NOW:\n");
	for (i = 0; i < 20; i++) {
		num = sadc_one(0x00, &spread, &avg);
		if (num <= 0) {
			seq_printf(s, " [%2d] read err/empty (num=%d)\n", i, num);
			continue;
		}
		seq_printf(s, " [%2d] AC spread=%6d (n=%d)\n", i, spread, num);
		if (spread < smin)
			smin = spread;
		if (spread > smax)
			smax = spread;
		ssum += spread;
		n++;
	}
	if (n)
		seq_printf(s, "AC spread: min=%d max=%d mean=%ld  (large+varying => mic IS on the line)\n",
			   smin, smax, ssum / n);
	seq_puts(s, "Metallic loop current (off-hook handset draws current):\n");
	for (i = 0; i < 3; i++) {
		num = sadc_one(0x07, &spread, &avg);
		seq_printf(s, " [%d] metallic avg=%d spread=%d (n=%d)\n",
			   i, avg, spread, num);
	}
	mutex_unlock(&sd.lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sadc_probe);

/*
 * Play the melody out the earpiece. Line up the SLIC if needed (audio_setup
 * now bakes the working slot-4 / linear / GR values) then loop the tune ~27s.
 * Does NOT re-poke TXSLOT/RXSLOT/OPFUNC after setup, so live poke experiments
 * survive a play.
 */
static int slic_play_show(struct seq_file *s, void *unused)
{
	int i, ret = 0;

	mutex_lock(&sd.lock);
	if (!slic_audio_up) {
		zsi_hw_init();
		zsi_slic_reset();
		writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
		slic_load_profiles();
		slic_audio_up = (slic_audio_setup() == 0);
	}
	mutex_unlock(&sd.lock);

	for (i = 0; i < 6; i++) {
		ret = pcm_en751221_play_melody();
		if (ret)
			break;
	}
	seq_printf(s, "play_melody loops=%d ret=%d (listen on the earpiece)\n",
		   i, ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_play);

/*
 * Digital loopback diagnostic: arm the SLIC TSA loopback (OPCOND bit 0x04) so
 * what the SoC transmits returns on the PCM bus, then TX a ramp + capture RX
 * ch2. A ramp coming back proves the TX digital path/slot map works and the
 * silence is purely the analog earpiece.
 */
static int count_ramp(const u8 *b)
{
	int i, n = 0;

	for (i = 1; i < 80; i++)
		if (b[i] == (u8)(b[i - 1] + 1))
			n++;
	return n;
}

static int slic_loopback_show(struct seq_file *s, void *unused)
{
	u8 out[160], v = 0xff;
	int ret;

	/* TEST 1: PCM-internal loopback (INTFACE bit25), SLIC out of the path.
	 * Proves the TX DMA in this code path actually transmits a ramp. */
	memset(out, 0, sizeof(out));
	ret = pcm_en751221_loopback_capture(out, 0xf5071306 | (1u << 25));
	seq_printf(s, "[internal] ret=%d ramp=%2d/79 %s\n", ret, count_ramp(out),
		   (count_ramp(out) > 60) ? "TX DMA OK" : "TX DMA broken");
	seq_printf(s, "  ch2: %32ph\n", out);

	/* TEST 2: SLIC TSA loopback (INTFACE normal, OPCOND bit 0x04). Proves
	 * the SoC TX -> bus -> SLIC -> bus -> SoC RX path. */
	mutex_lock(&sd.lock);
	if (!slic_audio_up) {
		zsi_hw_init();
		zsi_slic_reset();
		writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
		slic_load_profiles();
		slic_audio_up = (slic_audio_setup() == 0);
	}
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ static const u8 rx[] = { 0x42, 0x06 }; slic_write_mpi("rxslot6", rx, sizeof(rx)); }
	zsi_mpi_read((u8)slic_ec, 0x71, &v, 1);
	v |= 0x04;
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ u8 oc[2] = { 0x70, v }; slic_write_mpi("opcond-lb", oc, 2); }
	mutex_unlock(&sd.lock);

	memset(out, 0, sizeof(out));
	ret = pcm_en751221_loopback_capture(out, 0xf5071306);

	mutex_lock(&sd.lock);
	zsi_mpi_read((u8)slic_ec, 0x71, &v, 1);
	v &= ~0x04;
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ u8 oc[2] = { 0x70, v }; slic_write_mpi("opcond-rst", oc, 2); }
	mutex_unlock(&sd.lock);

	seq_printf(s, "[slic-tsa] ret=%d ramp=%2d/79 %s\n", ret, count_ramp(out),
		   (count_ramp(out) > 60) ? "SLIC BUS PATH OK" : "no ramp via SLIC");
	seq_printf(s, "  ch2: %32ph\n", out);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_loopback);

static int slic_write_ec1_mpi(const u8 *b, int n)
{
	int ret;

	ret = zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	if (ret)
		return ret;
	return slic_write_mpi("ec1-mpi", b, n);
}

static int slic_audio_slot4_prepare(u8 *opfunc_out, u8 *opcond_out,
				    u8 *state_before, bool *state_written)
{
	u8 v, st;
	int ret;

	ret = slic_write_ec1_mpi((const u8[]){ 0x40, 0x04 }, 2);
	if (ret)
		return ret;
	ret = slic_write_ec1_mpi((const u8[]){ 0x42, 0x04 }, 2);
	if (ret)
		return ret;

	ret = zsi_mpi_read((u8)slic_ec, 0x61, &v, 1);
	if (ret)
		return ret;
	v = (v & ~0xc0) | 0x80;
	ret = slic_write_ec1_mpi((const u8[]){ 0x60, v }, 2);
	if (ret)
		return ret;
	*opfunc_out = v;

	ret = slic_write_ec1_mpi((const u8[]){ 0x82, 0x40, 0x00 }, 3);
	if (ret)
		return ret;

	ret = zsi_mpi_read((u8)slic_ec, 0x71, &v, 1);
	if (ret)
		return ret;
	v &= ~0xc4;
	ret = slic_write_ec1_mpi((const u8[]){ 0x70, v }, 2);
	if (ret)
		return ret;
	*opcond_out = v;

	ret = zsi_mpi_read((u8)slic_ec, 0x57, &st, 1);
	if (ret)
		return ret;
	*state_before = st;
	*state_written = false;
	if (st != 0x23) {
		ret = slic_write_ec1_mpi((const u8[]){ 0x56, 0x23 }, 2);
		if (ret)
			return ret;
		*state_written = true;
	}
	slic_audio_up = true;
	return 0;
}

static int slic_gr_check_show(struct seq_file *s, void *unused)
{
	u8 gr_before[2] = { 0xee, 0xee }, gr_after[2] = { 0xee, 0xee };
	u8 opfunc = 0xee, tx = 0xee, rx = 0xee, opcond = 0xee, state = 0xee;
	int ret1, ret2;

	mutex_lock(&sd.lock);
	zsi_hw_init();
	writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
	ret1 = zsi_mpi_read((u8)slic_ec, 0x83, gr_before, 2);
	zsi_mpi_read((u8)slic_ec, 0x61, &opfunc, 1);
	ret2 = slic_write_ec1_mpi((const u8[]){ 0x82, 0x40, 0x00 }, 3);
	if (!ret2)
		ret2 = zsi_mpi_read((u8)slic_ec, 0x83, gr_after, 2);
	zsi_mpi_read((u8)slic_ec, 0x41, &tx, 1);
	zsi_mpi_read((u8)slic_ec, 0x43, &rx, 1);
	zsi_mpi_read((u8)slic_ec, 0x71, &opcond, 1);
	zsi_mpi_read((u8)slic_ec, 0x57, &state, 1);
	mutex_unlock(&sd.lock);

	seq_printf(s, "GR before ret=%d raw=%02x %02x be=0x%02x%02x le=0x%02x%02x\n",
		   ret1, gr_before[0], gr_before[1], gr_before[0], gr_before[1],
		   gr_before[1], gr_before[0]);
	seq_printf(s, "GR write sequence: EC 4a 01 ; MPI 82 40 00\n");
	seq_printf(s, "GR after ret=%d raw=%02x %02x be=0x%02x%02x le=0x%02x%02x %s\n",
		   ret2, gr_after[0], gr_after[1], gr_after[0], gr_after[1],
		   gr_after[1], gr_after[0],
		   (!ret2 && gr_after[0] == 0x40 && gr_after[1] == 0x00) ?
		   "CONFIRMED_0x4000" : "NOT_CONFIRMED");
	seq_printf(s, "OPFUNC raw=0x%02x %s  TXSLOT=0x%02x RXSLOT=0x%02x OPCOND=0x%02x STATE=0x%02x\n",
		   opfunc, ((opfunc & 0xc0) == 0x80) ? "LINEAR" : "NOT_LINEAR",
		   tx, rx, opcond, state);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_gr_check);

static int slic_tone_loopback_show(struct seq_file *s, void *unused)
{
	static u8 out[8 * 160];
	u8 v = 0xff, opfunc = 0xff, opcond = 0xff, state = 0xff;
	bool state_wr = false;
	int ret, ch;

	memset(out, 0, sizeof(out));
	mutex_lock(&sd.lock);
	zsi_hw_init();
	writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
	ret = slic_audio_slot4_prepare(&opfunc, &opcond, &state, &state_wr);
	if (!ret) {
		ret = zsi_mpi_read((u8)slic_ec, 0x71, &v, 1);
		if (!ret) {
			v = (v & ~0xc0) | 0x04;
			ret = slic_write_ec1_mpi((const u8[]){ 0x70, v }, 2);
		}
	}
	mutex_unlock(&sd.lock);

	if (!ret)
		ret = pcm_en751221_loopback_tone_capture(out, sizeof(out));

	mutex_lock(&sd.lock);
	zsi_mpi_read((u8)slic_ec, 0x71, &v, 1);
	v &= ~0x04;
	slic_write_ec1_mpi((const u8[]){ 0x70, v }, 2);
	mutex_unlock(&sd.lock);

	seq_printf(s, "tone_loopback ret=%d setup_opfunc=0x%02x setup_opcond=0x%02x state_before=0x%02x state_write=%s\n",
		   ret, opfunc, opcond, state, state_wr ? "56 23" : "none");
	seq_printf(s, "sequence: EC 4a 01 ; 40 04 ; 42 04 ; 60 %02x ; 82 40 00 ; 70 %02x ; loopback 70 %02x ; restore 70 %02x\n",
		   opfunc, opcond, (opcond & ~0xc0) | 0x04, v);
	for (ch = 0; ch < 8; ch++)
		seq_printf(s, "rx_ch%d_first32: %32ph\n", ch, out + ch * 160);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_tone_loopback);

static int slic_play_tone_show(struct seq_file *s, void *unused)
{
	u8 opfunc = 0xff, opcond = 0xff, state = 0xff;
	bool state_wr = false;
	int ret;

	mutex_lock(&sd.lock);
	zsi_hw_init();
	writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
	ret = slic_audio_slot4_prepare(&opfunc, &opcond, &state, &state_wr);
	mutex_unlock(&sd.lock);

	if (!ret)
		ret = pcm_en751221_play_tone_1khz();

	seq_printf(s, "play_tone_1khz ret=%d duration=10s\n", ret);
	seq_printf(s, "sequence: EC 4a 01 ; 40 04 ; 42 04 ; 60 %02x ; 82 40 00 ; 70 %02x ; %s ; PCM TX 1kHz 16-bit BE ; TX stop\n",
		   opfunc, opcond, state_wr ? "56 23" : "STATE already 23");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_play_tone);

static int slic_init_show(struct seq_file *s, void *unused)
{
	u8 id[2] = { 0xee, 0xee };
	u8 devmode = 0xee;
	int ret = slic_init(id, &devmode);

	seq_printf(s, "init ret=%d  RCNPCN=%02x %02x (expect 08 75)  devmode=0x%02x (expect 0x14)\n",
		   ret, id[0], id[1], devmode);
	if (!ret && id[0] == VP886_R_RCNPCN_RCN &&
	    id[1] == VP886_R_RCNPCN_PCN_LE9642 && devmode == 0x14)
		seq_puts(s, "Le9642 init + device/DC/AC/ring profiles loaded: OK\n");
	else
		seq_puts(s, "init: FAILED\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_init);

#define SLIC_DETECT_NBYTES	4

static int slic_detect_show(struct seq_file *s, void *unused)
{
	u8 id[SLIC_DETECT_NBYTES];
	int ret;

	memset(id, 0xee, sizeof(id));
	ret = slic_detect(id, sizeof(id));

	seq_printf(s, "RCNPCN(ZSI) ret=%d bytes= %*ph (expect 0x08 0x75)\n",
		   ret, (int)sizeof(id), id);
	if (!ret && id[0] == VP886_R_RCNPCN_RCN &&
	    id[1] == VP886_R_RCNPCN_PCN_LE9642)
		seq_puts(s, "Le9642 detected: OK\n");
	else
		seq_puts(s, "Le9642 detect: FAILED\n");

	seq_printf(s, "pcm: intface=0x%08x txslot=%4ph rxslot=%4ph\n",
		   readl(sd.pcm + PCM_INTFACE_CTRL),
		   sd.pcm + PCM_TX_TIME_SLOT_CFG0, sd.pcm + PCM_RX_TIME_SLOT_CFG0);
	seq_printf(s, "zsi: cfg=0x%08x ctl=0x%08x en=0x%08x  sys094=0x%08x iomux=0x%08x\n",
		   readl(sd.zsi + ZSI_CFG), readl(sd.zsi + ZSI_CTL),
		   readl(sd.zsi + ZSI_EN), readl(sd.sys + SYS_SPI_MODE),
		   readl(sd.chip_scu + SCU_IOMUX_CONTROL1));

	pr_info(DRV_NAME ": detect ret=%d bytes=%*ph\n", ret, (int)sizeof(id), id);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_detect);

/*
 * Earpiece tone via the SLIC's analog signal generator (SIGGEN) -- the path
 * that actually works (confirmed audible). NO PCM/codec/DMA/timeslots: gen A
 * is summed straight into the line driver, so an off-hook phone hears it just
 * like dial tone. Opcodes from vp886_registers.h (Le9662): SIGAB 0xD2 (11B:
 * ctrl + bias + freqA/ampA + freqB/ampB), SIGCTRL 0xDE (bit0 = enable gen A).
 * Tone code = Hz * 2.7307; 1 kHz = 0x0AAB. Amplitude 0x7000 (single tone, no
 * sum-clip). `cat tone` beeps for 3 s. Needs the line in a codec-on state
 * (STATE 0x23), which slic_audio_setup leaves it in.
 */
static int slic_tone_show(struct seq_file *s, void *unused)
{
	mutex_lock(&sd.lock);
	if (!slic_audio_up) {
		zsi_hw_init();
		zsi_slic_reset();
		writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
		slic_load_profiles();
		slic_audio_up = (slic_audio_setup() == 0);
	}
	/* SIGAB: gen A 1 kHz (0x0AAB), amp 0x7000, sine continuous; gen B off */
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ static const u8 sg[] = { 0xd2, 0x00, 0x00, 0x00, 0x0a, 0xab,
				   0x70, 0x00, 0x00, 0x00, 0x00, 0x00 };
	  slic_write_mpi("sigab", sg, sizeof(sg)); }
	/* SIGCTRL: enable gen A, continuous */
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ static const u8 on[] = { 0xde, 0x01 }; slic_write_mpi("sigctrl-on", on, sizeof(on)); }
	mutex_unlock(&sd.lock);

	msleep(3000);				/* beep duration */

	mutex_lock(&sd.lock);
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ (u8)slic_ec }, 1);
	{ static const u8 off[] = { 0xde, 0x00 }; slic_write_mpi("sigctrl-off", off, sizeof(off)); }
	mutex_unlock(&sd.lock);

	seq_puts(s, "tone: 1kHz @ amp 0x7000 via SIGGEN gen A, 3s beep into the line\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_tone);

/*
 * Real-time full-duplex sidetone: line up for voice, then loop the mic back to
 * the earpiece for ~15s (you hear yourself, ~150ms delayed). Proves continuous
 * full-duplex PCM streaming -- the foundation for the voice char device.
 */
static int slic_voice_loopback_show(struct seq_file *s, void *unused)
{
	int ret;

	mutex_lock(&sd.lock);
	if (!slic_audio_up) {
		zsi_hw_init();
		zsi_slic_reset();
		writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
		slic_load_profiles();
		slic_audio_up = (slic_audio_setup() == 0);
	}
	mutex_unlock(&sd.lock);

	ret = pcm_en751221_voice_loopback(15);
	seq_printf(s, "voice_loopback ret=%d (speak into the mic, you hear yourself on the earpiece for ~15s)\n", ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_voice_loopback);

static int __init econet_slic_init(void)
{
	sd.chip_scu = ioremap(CHIP_SCU_PHYS, CHIP_SCU_SIZE);
	sd.sys = ioremap(SYS_PHYS, SYS_SIZE);
	sd.pcm = ioremap(PCM_PHYS, PCM_SIZE);
	sd.zsi = ioremap(ZSI_PHYS, ZSI_SIZE);
	if (!sd.chip_scu || !sd.sys || !sd.pcm || !sd.zsi)
		goto err;

	mutex_init(&sd.lock);
	INIT_DELAYED_WORK(&hook_poll_work, hook_poll_fn);
	sd.cs = 0;
	sd.where = "idle";

	sd.dbg = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("slic_detect", 0444, sd.dbg, NULL, &slic_detect_fops);
	debugfs_create_file("slic_init", 0444, sd.dbg, NULL, &slic_init_fops);
	debugfs_create_file("audio_capture", 0444, sd.dbg, NULL, &slic_audio_fops);
	debugfs_create_file("audio_setup", 0444, sd.dbg, NULL, &slic_audio_setup_fops);
	debugfs_create_file("rx_scan", 0444, sd.dbg, NULL, &rx_scan_fops);
	debugfs_create_file("swctrl", 0200, sd.dbg, NULL, &swctrl_fops);
	debugfs_create_file("state", 0200, sd.dbg, NULL, &state_fops);
	debugfs_create_file("swparam", 0200, sd.dbg, NULL, &swparam_fops);
	debugfs_create_file("swtiming", 0200, sd.dbg, NULL, &swtiming_fops);
	debugfs_create_file("mpi", 0200, sd.dbg, NULL, &mpi_fops);
	debugfs_create_file("mpi_rd", 0200, sd.dbg, NULL, &mpi_rd_fops);
	debugfs_create_file("lpm", 0200, sd.dbg, NULL, &lpm_fops);
	debugfs_create_file("ring", 0200, sd.dbg, NULL, &ring_fops);
	debugfs_create_file("hook", 0444, sd.dbg, NULL, &slic_hook_fops);
	debugfs_create_file("hook_raw", 0444, sd.dbg, NULL, &slic_hook_raw_fops);
	debugfs_create_file("sadc_probe", 0444, sd.dbg, NULL, &sadc_probe_fops);
	debugfs_create_file("play", 0444, sd.dbg, NULL, &slic_play_fops);
	debugfs_create_file("tx_loopback", 0444, sd.dbg, NULL, &slic_loopback_fops);
	debugfs_create_file("gr_check", 0444, sd.dbg, NULL, &slic_gr_check_fops);
	debugfs_create_file("tone_loopback", 0444, sd.dbg, NULL, &slic_tone_loopback_fops);
	debugfs_create_file("play_tone", 0444, sd.dbg, NULL, &slic_play_tone_fops);
	debugfs_create_file("tone", 0444, sd.dbg, NULL, &slic_tone_fops);
	debugfs_create_file("voice_loopback", 0444, sd.dbg, NULL, &slic_voice_loopback_fops);
	debugfs_create_u8("cs", 0644, sd.dbg, &sd.cs);
	debugfs_create_x32("pcm_intface", 0644, sd.dbg, &pcm_intface_ctrl);
	debugfs_create_x32("zsi_cfg", 0644, sd.dbg, &zsi_cfg_val);
	{
		static const char *tx_n[4] = { "tx_slot0", "tx_slot1", "tx_slot2", "tx_slot3" };
		static const char *rx_n[4] = { "rx_slot0", "rx_slot1", "rx_slot2", "rx_slot3" };
		int i;

		for (i = 0; i < 4; i++) {
			debugfs_create_x32(tx_n[i], 0644, sd.dbg, &pcm_tx_slots[i]);
			debugfs_create_x32(rx_n[i], 0644, sd.dbg, &pcm_rx_slots[i]);
		}
	}

	if (misc_register(&voice_miscdev))
		pr_warn(DRV_NAME ": /dev/xr500v-voice register failed\n");

	pr_info(DRV_NAME ": loaded ZSI mode (cat /sys/kernel/debug/%s/slic_detect)\n",
		DRV_NAME);
	return 0;
err:
	if (sd.zsi)
		iounmap(sd.zsi);
	if (sd.pcm)
		iounmap(sd.pcm);
	if (sd.sys)
		iounmap(sd.sys);
	if (sd.chip_scu)
		iounmap(sd.chip_scu);
	return -ENOMEM;
}

static void __exit econet_slic_exit(void)
{
	WRITE_ONCE(hook_poll_running, false);
	cancel_delayed_work_sync(&hook_poll_work);
	pcm_en751221_voice_stop();
	misc_deregister(&voice_miscdev);
	debugfs_remove_recursive(sd.dbg);
	iounmap(sd.zsi);
	iounmap(sd.pcm);
	iounmap(sd.sys);
	iounmap(sd.chip_scu);
}

module_init(econet_slic_init);
module_exit(econet_slic_exit);

MODULE_DESCRIPTION("EcoNet EN751221 Le9642 SLIC ZSI chip detect (phase 3a)");
MODULE_LICENSE("GPL");

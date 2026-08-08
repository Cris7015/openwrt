/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register map for the EcoNet/TrendChip EN751221 PCM/TDM controller.
 *
 * Reconstructed from the OEM Trendchip 2.6.36 pcmdriver.h plus disassembly of
 * the stock pcm1.ko (mips-linux-gnu-objdump). The OEM header used KSEG1 virtual
 * addresses (0xbfbd0000 ...); here we use physical addresses since the driver
 * ioremaps them / takes the block from the DT reg resource.
 *
 * Two instances exist on the SoC:
 *   PCM0 @ phys 0x1fbd0000  (IRQ 12, reset bit 0x800)
 *   PCM1 @ phys 0x1fbd2000  (IRQ 34, reset bit 0x10)
 */
#ifndef _PCM_EN751221_H
#define _PCM_EN751221_H

#include <linux/bits.h>

/*
 * PCM register file: 17 sequential 32-bit registers at base + n*4.
 * Default (reset) values are from the OEM enum pcm_reg_num comments.
 */
enum pcm_reg {
	PCM_INTFACE_CTRL = 0,		/* default 0x0100040a */
	PCM_TX_TIME_SLOT_CFG0,		/* default 0x00080000 */
	PCM_TX_TIME_SLOT_CFG1,		/* default 0x00180010 */
	PCM_TX_TIME_SLOT_CFG2,		/* default 0x00280020 */
	PCM_TX_TIME_SLOT_CFG3,		/* default 0x00380030 */
	PCM_RX_TIME_SLOT_CFG0,		/* default 0x00080000 */
	PCM_RX_TIME_SLOT_CFG1,		/* default 0x00180010 */
	PCM_RX_TIME_SLOT_CFG2,		/* default 0x00280020 */
	PCM_RX_TIME_SLOT_CFG3,		/* default 0x00380030 */
	PCM_ISR,			/* 0x24 - interrupt status */
	PCM_IMR,			/* 0x28 - interrupt mask */
	PCM_TX_POLLING_DEMAND,		/* 0x2c */
	PCM_RX_POLLING_DEMAND,		/* 0x30 */
	PCM_TX_DESC_RING_BASE,		/* 0x34 */
	PCM_RX_DESC_RING_BASE,		/* 0x38 */
	PCM_TX_RX_DESC_RING_SIZE_OFFSET,/* 0x3c default 0x000000c0 */
	PCM_TX_RX_DMA_CTRL,		/* 0x40 default 0x0f000000 */
	PCM_REG_COUNT
};

#define PCM_REG(n)		((n) * 4)

/* DMA control bits (PCM_TX_RX_DMA_CTRL @ 0x40), from disassembly of dmaEnable() */
#define PCM_DMA_TX_EN		BIT(0)
#define PCM_DMA_RX_EN		BIT(1)
#define PCM_DMA_CH_VALID_MASK	GENMASK(31, 24)
#define PCM_DMA_CH_VALID_SHIFT	24

/* Interrupt bits (PCM_ISR / PCM_IMR) */
#define PCM_INT_TX_RX_FRAME_BOUNDARY	BIT(0)
#define PCM_INT_TX_DESC_UPDATE		BIT(2)
#define PCM_INT_RX_DESC_UPDATE		BIT(3)
#define PCM_INT_END_OF_TX_DESC		BIT(4)
#define PCM_INT_END_OF_RX_DESC		BIT(5)
#define PCM_INT_TX_BUF_UNDER_RUN	BIT(6)
#define PCM_INT_RX_BUF_OVER_RUN		BIT(7)
#define PCM_INT_AHB_BUS_ERR		BIT(8)

/*
 * SoC SCU block (DT: scu syscon@1fb00000, 0x970 long).
 * Holds the chip-id and the PCM soft-reset bit.
 */
#define SCU_PHYS_BASE		0x1fb00000
#define SCU_CHIP_ID		0x064	/* (val & 0xffff0000) == 0x00070000 -> EN751221 */
#define SCU_PCM_RESET		0x834	/* bit 0x800 resets PCM0 */

#define SCU_CHIP_ID_MASK	0xffff0000
#define SCU_CHIP_EN751221	0x00070000
#define SCU_CHIP_EN7526C	0x00080000
#define SCU_PCM0_RESET_BIT	0x800

/*
 * Chip SCU block (DT: chip_scu syscon@1fa20000, 0x388 long).
 * Holds the PCM/SLIC pinmux and clock source select. Used from phase 2.
 */
#define CHIP_SCU_PHYS_BASE	0x1fa20000
#define CHIP_SCU_IOMUX_CONTROL1	0x104
#define CHIP_SCU_PCM_CLK_DIV	0x0d4
#define CHIP_SCU_PCM_CLK_OUTPUT	0x0d8
#define CHIP_SCU_PCM_CLK_SRC_SEL 0x148

#define CHIP_SCU_PCM_PIN_MODE	BIT(12)
#define CHIP_SCU_GPIO_ZSI_ISI	BIT(13)
#define CHIP_SCU_PCM_CLK_OUT_EN	BIT(0)
#define CHIP_SCU_PCM_ZSI_CLK_SRC GENMASK(3, 2)
#define CHIP_SCU_PCM_ZSI_CLK_SRC_VAL 0x3

#define MAX_BUF_NUM		8
#define MAX_CH_NUM		8
#define PCM_DESC_NUM		15	/* MAX_TX/RX_DESC_NUM (ring depth) */
#define PCM_DMA_ADDR_MASK	0x1fffffffULL

/*
 * PCM_INTFACE_CTRL @ 0x00, reconstructed from pcmConfigSetup().
 *
 * Claimed fields and backing instructions:
 *   bit 31    CONFIG_VALID/latch: set to 1 at pcm1.ko 0x2bfc
 *   bit 25    LOOPBACK_EN: loopbackMode < 2 copied at pcm1.ko 0x2c98/0x2c9c.
 *             BACK_TO_BACK is loopbackMode=1, therefore bit 25 = 1.
 *   bits 22:18 FRAME_COUNT: configNode.frameCount, pcm1.ko 0x2c18
 *   bit 17    BYTE_ORDER: configNode.byteOrder, pcm1.ko 0x2c4c
 *   bit 16    BIT_ORDER: configNode.bitOrder, pcm1.ko 0x2ca4
 *   bit 12    BIT_DELAY: forced to 1, pcm1.ko 0x2c00
 *   bit 11    DATA_EDGE: forced to 0, pcm1.ko 0x2c08
 *   bit 10    FS_EDGE: configNode.fsEdge, pcm1.ko 0x2cc4
 *   bits 9:8  FS_LEN: configNode.fsLen 0/2/3, pcm1.ko 0x2d50/0x2e28
 *   bit 4     SAMPLE_CLOCK: configNode.sampleClock, pcm1.ko 0x2ce4
 *   bits 3:1  BIT_CLOCK: configNode.bitClock, pcm1.ko 0x2cf0
 *   bit 0     PCM_MODE: configNode.pcmMode, pcm1.ko 0x2d24
 *
 * chNum is not in INTFACE_CTRL. pcmConfigSetup() writes ((1 << chNum) - 1)
 * into TX_RX_DMA_CTRL[31:24] at pcm1.ko 0x2d74..0x2da0.
 *
 * UNCONFIRMED: OEM pcm_ext_conf.h names bit 31 lbGarbageEnable and bit 26
 * cfgValid due C bitfield ordering, but the blob itself toggles bit 31 as the
 * configuration latch. Hardware naming should be checked against a datasheet.
 */
#define PCM_IFACE_CONFIG_VALID	BIT(31)
#define PCM_IFACE_LOOPBACK_EN	BIT(25)
#define PCM_IFACE_FRAME_COUNT	GENMASK(22, 18)
#define PCM_IFACE_BYTE_ORDER	BIT(17)
#define PCM_IFACE_BIT_ORDER	BIT(16)
#define PCM_IFACE_BIT_DELAY	BIT(12)
#define PCM_IFACE_DATA_EDGE	BIT(11)
#define PCM_IFACE_FS_EDGE	BIT(10)
#define PCM_IFACE_FS_LEN	GENMASK(9, 8)
#define PCM_IFACE_SAMPLE_CLOCK	BIT(4)
#define PCM_IFACE_BIT_CLOCK	GENMASK(3, 1)
#define PCM_IFACE_PCM_MODE	BIT(0)

#define PCM_LOOPBACK_NONE	0
#define PCM_LOOPBACK_BACK_TO_BACK 1
#define PCM_LOOPBACK_ONE_BY_ONE	2
#define PCM_LOOPBACK_HEADER_PATTERN 0x12345678

#define PCM_SELFTEST_CHANS	4
#define PCM_SELFTEST_CH_VALID	GENMASK(PCM_SELFTEST_CHANS - 1, 0)
#define PCM_SELFTEST_SAMPLES	80
#define PCM_SELFTEST_BYTES_PER_SAMPLE 2
#define PCM_SELFTEST_CH_BYTES \
	(PCM_SELFTEST_SAMPLES * PCM_SELFTEST_BYTES_PER_SAMPLE)
#define PCM_SELFTEST_BUF_BYTES \
	(PCM_SELFTEST_CHANS * PCM_SELFTEST_CH_BYTES)

/* TX descriptor status word (PCM_TX/RX_DESC_RING_BASE entries). */
union pcm_desc_status {
	struct {
		u32 ownership : 1;
		u32 reserved : 7;
		u32 ch_valid : 8;
		u32 reserved2 : 6;
		u32 sample_size : 10;
	} bits;
	u32 value;
};

/* One DMA descriptor: status + up to MAX_BUF_NUM buffer pointers. */
struct pcm_desc {
	u32 status;
	u32 buf_addr[MAX_BUF_NUM];
};

#define PCM_DESC_OWN		BIT(31)
#define PCM_DESC_CH_VALID	GENMASK(23, 16)
#define PCM_DESC_SAMPLE_SIZE	GENMASK(9, 0)

/* Time-slot config register layout (PCM_TX/RX_TIME_SLOT_CFGn). */
union pcm_slot_cfg {
	struct {
		u32 reserved : 3;
		u32 bit_width_next : 1;
		u32 reserved2 : 2;
		u32 bit_counter_next : 10;
		u32 reserved3 : 3;
		u32 bit_width : 1;
		u32 reserved4 : 2;
		u32 bit_counter : 10;
	} bits;
	u32 value;
};

/*
 * Bring the PCM up generating continuous PCLK/FSYNC (DMA armed) for a ZSI SLIC.
 * Exported by the PCM driver, called by the SLIC (ZSI) driver.
 */
int pcm_en751221_zsi_clock_run(u32 intface, const u32 *tx_slots,
			       const u32 *rx_slots);

/* Capture one ~10ms voice frame (slot 6 = SLIC mic) into out (max 80 bytes). */
int pcm_en751221_capture_rx(u8 *out, int nbytes);

/* Diagnostic: capture all 8 RX channels (OEM slot map) into out[8*80]. */
int pcm_en751221_capture_allch(u8 *out);

/* Play a mu-law melody out the TX path (broadcast to all 8 slots). Blocks. */
int pcm_en751221_play_melody(void);

/* Real-time full-duplex sidetone loopback (mic -> earpiece) for `seconds`. */
int pcm_en751221_voice_loopback(int seconds);

/* Continuous full-duplex voice streaming engine (backs /dev/xr500v-voice). */
int pcm_en751221_voice_start(void);
void pcm_en751221_voice_reset(void);
void pcm_en751221_voice_set_mute(int on);
void pcm_en751221_voice_stop(void);
ssize_t pcm_en751221_voice_read(char __user *ubuf, size_t len);
ssize_t pcm_en751221_voice_write(const char __user *ubuf, size_t len);

/* Play a sustained 1 kHz signed 16-bit big-endian linear tone. Blocks. */
int pcm_en751221_play_tone_1khz(void);

/* Loopback: TX a ramp + capture RX ch2. intface picks PCM-internal loopback
 * (bit 25 set) vs external/SLIC loopback (bit 25 clear). */
int pcm_en751221_loopback_capture(u8 *out, u32 intface);

/* Loopback: TX 1 kHz signed 16-bit BE tone and capture RX ch0. */
int pcm_en751221_loopback_tone_capture(u8 *out, int nbytes);

#endif /* _PCM_EN751221_H */

// SPDX-License-Identifier: GPL-2.0-only
/*
 * TP-Link Archer XR500v / EcoNet EN751221 guarded GPON O3 RX lab.
 *
 * The proven O2 trigger and first two FIFO pops are retained verbatim.  After
 * both records are proved to be identical broadcast Upstream_Overhead
 * messages, their receive-only formatter parameters are applied to the
 * audited OEM MAC/xPON registers and the local MAC/PHY state is advanced to
 * O3.  GPIO16 TX_DISABLE remains asserted while PHYSET3 remains in its
 * audited GPON burst-mode configuration for the entire transaction.  A
 * bounded O3 window drains complete downstream
 * records, classifies Extended_Burst_Length without applying its formatter,
 * and correlates a MAC TX-GEM counter with raw, unlatched, non-decisional PHY
 * diagnostics while observing serial-number request/internal events without
 * writing an identity, an upstream FIFO, interrupt enables, EN7570 or laser
 * state.
 * Exact restoration is attempted only behind fail-closed guards; any
 * unproved boundary pins the module for a power cut.  Raw PLOAM words are
 * available only from a root-only debugfs file.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/preempt.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>

#define EN751221_SCU_WAN_CONF		0x070
#define EN751221_SCU_RESET_CTRL2		0x830
#define EN751221_SCU_RESET_CTRL1		0x834

#define XR500V_XPON_PHYS		0x1faf0000
#define XR500V_XPON_SIZE		0x00001000
#define XR500V_TX_DISABLE_GPIO		528

#define XPON_PHYSET2			0x0104
#define XPON_PHYSET3			0x0108
#define XPON_PHYSET10			0x0124
#define XPON_PHYSTA1			0x0130
#define XPON_SETTING			0x0138
#define XPON_MISC			0x01fc
#define XPON_PHYRX_STATUS		0x021c
#define XPON_GPON_PREAMBLE		0x0400
#define XPON_GPON_DELIMITER_GUARD	0x0404
#define XPON_GPON_EXT_PREAMBLE		0x0408
#define XPON_PHY_TX_STATUS_RAW		0x040c
#define XPON_PHY_TX_FRAME_COUNTER_RAW	0x0434
#define XPON_PHY_TX_BURST_COUNTER_RAW	0x0438
#define XPON_BISTCTL_PRBS_TX_EN		0x04a4
#define XPON_TEST_FRAME_EN		0x0510
#define XPON_TRANS_STATUS		0x05e0
#define XPON_INT_ENABLE			0x05f0
#define XPON_INT_STATUS			0x05f8

#define PHYSET2_FW_READY		BIT(0)
#define PHYSET2_STATUS_BIT		BIT(2)
#define PHYSET3_CONTINUOUS_MODE		BIT(5)
#define PHYSET10_GPON_MODE		BIT(31)
#define PHYSTA1_STATE			GENMASK(20, 18)
#define PHYSTA1_READY			6
#define XPON_SETTING_EN7570		0x0000010f
#define XPON_MISC_ROGUE_TX		BIT(28)
#define PHYRX_SYNC			GENMASK(3, 0)
#define PHYRX_SYNC_GPON			0x0a
#define PHYRX_FEC			BIT(15)
#define TRANS_STATUS_LOS		BIT(0)

#define GPON_MAC_PHYS			0x1fb64000
#define GPON_MAC_SIZE			0x00000400
#define GPON_G_ONU_ID			0x000
#define GPON_G_GBL_CFG			0x004
#define GPON_G_INT_STATUS		0x008
#define GPON_G_INT_ENABLE		0x00c
#define GPON_G_PLOAMU_FIFO_STS		0x050
#define GPON_G_PLOAMD_FIFO_STS		0x058
#define GPON_G_PLOAMD_RDATA		0x05c
#define GPON_G_PLOU_GUARD_BIT		0x094
#define GPON_G_PLOU_PREAMBLE12		0x098
#define GPON_G_PLOU_PREAMBLE3		0x09c
#define GPON_G_PRE_ASSIGNED_DELAY	0x0a4
#define GPON_G_RSP_TIME			0x0ac
#define GPON_G_VENDOR_ID			0x0b0
#define GPON_G_VS_SN			0x0b4
#define GPON_G_SN_MSG_CFG		0x0b8
#define GPON_G_ACTIVATION_ST		0x0bc
#define GPON_G_MBI_STOP			0x160
#define GPON_DBG_CAP_SETTING		0x200
#define GPON_DBG_DLY			0x208
#define GPON_DBG_IDLE_GEM_THLD		0x20c
#define GPON_DBG_RX_GEM_CNT		0x300
#define GPON_DBG_RX_CRC_ERR_CNT		0x304
#define GPON_DBG_RX_GTC_CNT		0x308
#define GPON_DBG_TX_GEM_CNT		0x30c
#define GPON_DBG_TX_BST_CNT		0x310
#define GPON_DBG_RX_HEC_ONE_ERR_CNT	0x330
#define GPON_DBG_RX_HEC_TWO_ERR_CNT	0x334
#define GPON_DBG_RX_HEC_UNCORR_CNT	0x338
#define GPON_DBG_DS_SPF_CNT		0x358
#define GPON_DBG_PLOAMD_FILTER_IN_O5	0x360
#define GPON_O3_O4_PLOAMU_CTRL		0x3c4

/* bit 0 clear selects autonomous MAC PLOAMu handling in O3/O4. */
#define O3_O4_PLOAMU_CTRL_SW		BIT(0)

#define WAN_MODE			GENMASK(2, 0)
#define WAN_MODE_GPON			0
#define WAN_MODE_ATM			3
#define RESET2_XPON_PHY			BIT(0)
#define RESET1_QDMA1			BIT(1)
#define RESET1_QDMA2			BIT(2)
#define RESET1_FE			BIT(21)
#define RESET1_XPON_MAC			BIT(31)
#define REQUIRED_RELEASED_RESET1	(RESET1_QDMA1 | RESET1_QDMA2 | \
					 RESET1_FE | RESET1_XPON_MAC)

#define ONU_ID_VALID			BIT(15)
#define ONU_ID_VALUE			GENMASK(7, 0)
#define O3_RX_ID_CHANGE_ONU_ID		BIT(0)
#define O3_RX_ID_CHANGE_VENDOR_ID	BIT(1)
#define O3_RX_ID_CHANGE_VS_SN		BIT(2)

/*
 * G_SN_MSG_CFG has no identity payload.  Keep this diagnostic intentionally
 * coarse: it records only which public register field changed, never either
 * value or the XOR delta.
 */
#define O3_RX_SN_MSG_CHANGE_THRESHOLD	BIT(0)
#define O3_RX_SN_MSG_CHANGE_TX_POWER	BIT(1)
#define O3_RX_SN_MSG_CHANGE_RANDOM_DELAY	BIT(2)
#define O3_RX_SN_MSG_CHANGE_RESERVED	BIT(3)
#define O3_RX_SN_MSG_THRESHOLD		GENMASK(31, 24)
#define O3_RX_SN_MSG_TX_POWER		GENMASK(17, 16)
#define O3_RX_SN_MSG_RANDOM_DELAY	GENMASK(11, 0)
#define O3_RX_SN_MSG_KNOWN		(O3_RX_SN_MSG_THRESHOLD | \
					 O3_RX_SN_MSG_TX_POWER | \
					 O3_RX_SN_MSG_RANDOM_DELAY)

/*
 * Non-secret post-guard diagnostics.  These bits name only which invariant
 * changed; none of the identity words are exposed in status or logs.
 */
#define O3_RX_GUARD_DIAG_XPON		BIT(0)
#define O3_RX_GUARD_DIAG_ACTIVATION	BIT(1)
#define O3_RX_GUARD_DIAG_IRQ_ENABLE	BIT(2)
#define O3_RX_GUARD_DIAG_TX_BURST	BIT(3)
#define O3_RX_GUARD_DIAG_PLOAMU	BIT(4)
#define O3_RX_GUARD_DIAG_IDENTITY	BIT(5)
#define O3_RX_GUARD_DIAG_GLOBAL	BIT(6)
#define O3_RX_GUARD_DIAG_WAN		BIT(7)
#define O3_RX_GUARD_DIAG_SCU_READ	BIT(8)
#define O3_RX_GUARD_DIAG_PLOAMU_CTRL	BIT(9)
#define O3_RX_GUARD_DIAG_TX_STATUS	BIT(10)
#define O3_RX_GUARD_DIAG_TX_GEM		BIT(11)

#define PLOAM_FIFO_LEVEL		GENMASK(7, 0)
#define PLOAM_FIFO_MAX_USED		GENMASK(23, 16)
#define PLOAMU_FIFO_UNDERRUN		BIT(31)
#define PLOAMD_FIFO_OVERRUN		BIT(31)
#define ACTIVATION_STATE		GENMASK(2, 0)
#define ACTIVATION_O1			1
#define ACTIVATION_O2			2
#define ACTIVATION_O3			3
#define RESET_ONU_ID			0x000000ff
#define RESET_GLOBAL_CONFIG		0x00000034
#define RESET_PLOAMU_FIFO		0x00800080
#define INT_PLOAMD_RECV			BIT(0)
#define INT_PLOAMU_SEND			BIT(1)
#define INT_SN_REQ_RECV			BIT(2)
#define INT_SN_ONU_SEND_O3		BIT(3)
#define INT_RANGING_REQ_RECV		BIT(4)
#define INT_SN_ONU_SEND_O4		BIT(5)
#define INT_SN_REQ_CRS			BIT(6)
#define INT_DYING_GASP_SEND		BIT(11)
#define INT_RX_ERR			BIT(16)
#define INT_FIFO_ERR			BIT(17)
#define INT_BST_SGL_DIFF		BIT(18)
#define INT_TX_LATE_START		BIT(19)
#define INT_RX_EOF_ERR			BIT(20)
#define INT_RX_GEM_INTLV_ERR		BIT(21)
#define INT_BFIFO_FULL			BIT(22)
#define INT_SFIFO_FULL			BIT(23)
#define INT_O5_EQD_ADJ_DONE		BIT(24)
#define INT_OLT_DS_FEC_CHG		BIT(25)
#define INT_ONU_US_FEC_CHG		BIT(26)
#define INT_POPUP_RECV_O6		BIT(27)
#define INT_FWI				BIT(28)
#define INT_LWI				BIT(29)
#define INT_BWM_STOP_TIME_ERR		BIT(30)
#define INT_BWM_US_FEC_ERR		BIT(31)

#define GPON_FIRST_GTC_TIMEOUT_US	5000
#define GPON_TOTAL_OBSERVE_US		15000000
#define GPON_O2_HARD_LIMIT_US		15250000
#define GPON_FIRST_POLL_MIN_US		100
#define GPON_FIRST_POLL_MAX_US		200
#define GPON_POLL_MIN_US		1500
#define GPON_POLL_MAX_US		2500
#define GPON_POLL_HARD_GAP_US		25000
#define GPON_PASSIVE_STARTUP_SETTLE_US	1100000
#define INT_TX_ACTIVITY			(INT_PLOAMU_SEND | \
					 INT_SN_ONU_SEND_O3 | \
					 INT_SN_ONU_SEND_O4 | \
					 INT_DYING_GASP_SEND | \
					 INT_TX_LATE_START)
#define O3_RX_DS_FIFO_LEVEL		BIT(0)
#define O3_RX_DS_PLOAMD_STATUS		BIT(1)
#define O3_RX_DS_SN_REQ		BIT(2)
#define O3_RX_DS_RANGING_REQ		BIT(3)
#define O3_RX_DS_SN_REQ_CRS		BIT(4)
#define O3_RX_SAFE_STARTUP_MASK	(INT_RX_ERR | INT_RX_EOF_ERR | \
					 INT_OLT_DS_FEC_CHG | INT_LWI)
#define O3_RX_SAFE_DOWNSTREAM_IRQ_MASK	(INT_PLOAMD_RECV | \
					 INT_SN_REQ_RECV | \
					 INT_RANGING_REQ_RECV | \
					 INT_SN_REQ_CRS)
#define O3_RX_O3_ALLOWED_STATUS		(O3_RX_SAFE_STARTUP_MASK | \
					 O3_RX_SAFE_DOWNSTREAM_IRQ_MASK)
#define O3_RX_CLEANUP_LATCH_DELTA	BIT(0)
#define O3_RX_CLEANUP_LATCH_XPON	BIT(1)
#define O3_RX_CLEANUP_LATCH_STATE	BIT(2)
#define O3_RX_CLEANUP_LATCH_IRQ_ENABLE	BIT(3)
#define O3_RX_CLEANUP_LATCH_TX_STATUS	BIT(4)
#define O3_RX_CLEANUP_LATCH_PLOAMU	BIT(5)
#define O3_RX_CLEANUP_LATCH_PLOAMU_CTRL	BIT(6)
#define O3_RX_CLEANUP_LATCH_IDENTITY	BIT(7)
#define O3_RX_CLEANUP_LATCH_GLOBAL	BIT(8)
#define O3_RX_CLEANUP_LATCH_STATUS	BIT(9)
#define O3_RX_CLEANUP_LATCH_DS_OVERRUN	BIT(10)
#define O3_RX_CLEANUP_LATCH_TX_GEM	BIT(11)
#define O3_RX_CLEANUP_LATCH_SOURCE_CLASS	BIT(12)
#define O3_RX_CLEANUP_LATCH_PLOAMU_DELTA	BIT(13)
#define O3_RX_CLEANUP_LATCH_DS_PARTIAL	BIT(14)
#define O3_RX_UPSTREAM_LATCH_TX_STATUS	BIT(0)
#define O3_RX_UPSTREAM_LATCH_TX_BURST	BIT(1)
#define O3_RX_UPSTREAM_LATCH_PLOAMU	BIT(2)
#define O3_RX_UPSTREAM_LATCH_TX_GEM	BIT(3)
#define O3_RX_EXPECTED_STARTUP_MASK	O3_RX_SAFE_STARTUP_MASK
#define O3_RX_BASELINE_PRE_DS_FEC	(INT_RX_ERR | INT_LWI)
#define O3_RX_BASELINE_NO_EOF		(INT_RX_ERR | INT_OLT_DS_FEC_CHG | INT_LWI)
#define O3_RX_BASELINE_EOF_LATCHED	O3_RX_EXPECTED_STARTUP_MASK
#define O3_RX_PASSIVE_IDLE_STATUS	O3_RX_EXPECTED_STARTUP_MASK
#define O3_RX_PASSIVE_TRIGGER_STATUS	(O3_RX_PASSIVE_IDLE_STATUS | \
						 INT_PLOAMD_RECV)
#define O3_RX_EXACT_TRIGGER_SOURCE	(O3_RX_DS_FIFO_LEVEL | \
					 O3_RX_DS_PLOAMD_STATUS)
#define O3_RX_FIFO_BEFORE_POP		0x00090009
#define O3_RX_FIFO_AFTER_FIRST_POP	0x00090006
#define O3_RX_FIFO_AFTER_SECOND_POP	0x00090003
#define O3_RX_PLOAMD_WORDS_PER_RECORD	3
#define O3_RX_INITIAL_RECORDS		2
#define O3_RX_MAX_RECORDS		32
#define O3_RX_PLOAMD_TOTAL_WORDS		(O3_RX_PLOAMD_WORDS_PER_RECORD * \
					 O3_RX_INITIAL_RECORDS)
#define O3_RX_PLOAMD_BYTES_PER_RECORD	12
#define O3_RX_FIFO_POP_HARD_US		125
#define O3_RX_DEFAULT_MAX_RECORDS	16
#define O3_RX_DEFAULT_OBSERVE_MS		2000
#define O3_RX_MIN_OBSERVE_MS		250
#define O3_RX_MAX_OBSERVE_MS		5000
#define O3_RX_POLL_MIN_US		500
#define O3_RX_POLL_MAX_US		1000
#define O3_RX_MAX_POLL_COUNT		10000
#define O3_RX_PLOAM_BROADCAST		0xff
#define O3_RX_PLOAM_UPSTREAM_OVERHEAD	0x01
#define O3_RX_PLOAM_EXT_BURST_LENGTH	0x14
#define O3_RX_PHY_GUARD_BITS		24
#define O3_RX_PHY_GUARD_PATTERN		0xaa
#define O3_RX_PHY_OPER_RANGED		GENMASK(18, 17)
#define O3_RX_PHY_OPER_O3_O4		2
#define O3_RX_MAC_GUARD_BITS		GENMASK(7, 0)
#define O3_RX_MAC_PREAMBLE1		GENMASK(7, 0)
#define O3_RX_MAC_PREAMBLE2		GENMASK(15, 8)
#define O3_RX_MAC_PRE_DELAY		GENMASK(15, 0)
#define O3_RX_MAC_PRE_DELAY_ENABLE	BIT(31)

enum o3_rx_stop_reason {
	O3_RX_REASON_NONE,
	O3_RX_REASON_PREFLIGHT_XPON_GUARD,
	O3_RX_REASON_PREFLIGHT_SCU_READ,
	O3_RX_REASON_PREFLIGHT_WAN_MODE,
	O3_RX_REASON_PREFLIGHT_RESET_STATE,
	O3_RX_REASON_TIMELINE_COMPLETE,
	O3_RX_REASON_FIRST_GTC_TIMEOUT,
	O3_RX_REASON_STARTUP_STATUS_UNSAFE,
	O3_RX_REASON_STARTUP_STATUS_UNEXPECTED,
	O3_RX_REASON_STARTUP_EOF_MISSING,
	O3_RX_REASON_STAGE_A_TARGET_NOT_CLEARED,
	O3_RX_REASON_STAGE_A_NON_TARGET_LOST,
	O3_RX_REASON_STAGE_B_TARGET_UNEXPECTED,
	O3_RX_REASON_STAGE_B_TARGET_NOT_CLEARED,
	O3_RX_REASON_STAGE_B_NON_TARGET_LOST,
	O3_RX_REASON_CLEAR_READBACK_SLOW,
	O3_RX_REASON_UNSAFE_INTERRUPT_STATUS,
	O3_RX_REASON_ABORT_REQUESTED,
	O3_RX_REASON_ACTIVATION_LEFT_O2,
	O3_RX_REASON_INTERRUPT_ENABLED,
	O3_RX_REASON_TX_ACTIVITY,
	O3_RX_REASON_TX_BURST_CHANGED,
	O3_RX_REASON_TX_BURST_PLOAMU_UNDERRUN,
	O3_RX_REASON_TX_GEM_CHANGED,
	O3_RX_REASON_ONU_ID_CHANGED,
	O3_RX_REASON_GLOBAL_CONFIG_CHANGED,
	O3_RX_REASON_PLOAMU_CHANGED,
	O3_RX_REASON_XPON_GUARD,
	O3_RX_REASON_POLL_GAP,
	O3_RX_REASON_MUX_CHANGED,
	O3_RX_REASON_MUX_SELECT_WRITE,
	O3_RX_REASON_MUX_SELECT_READBACK,
	O3_RX_REASON_PRE_OBSERVATION_GUARD,
	O3_RX_REASON_PRE_OBSERVATION_PRECONDITION,
	O3_RX_REASON_ACTIVATION_READBACK,
	O3_RX_REASON_DOWNSTREAM_PROGRESS,
	O3_RX_REASON_TRIGGER_NOT_EXACT,
	O3_RX_REASON_FINAL_PRE_POP_CHANGED,
	O3_RX_REASON_FIFO_CHANGED_AFTER_O1,
	O3_RX_REASON_PRE_POP_MUX_CHANGED,
	O3_RX_REASON_PRE_POP_GUARD,
	O3_RX_REASON_FIFO_POP_SLOW,
	O3_RX_REASON_FIFO_POST_STATUS,
	O3_RX_REASON_POST_POP_MUX_CHANGED,
	O3_RX_REASON_POST_POP_UNSAFE,
	O3_RX_REASON_FIRST_POP_BOUNDARY,
	O3_RX_REASON_TWO_POP_COMPLETE,
	O3_RX_REASON_INITIAL_RECORDS_INVALID,
	O3_RX_REASON_O3_O4_PLOAMU_HW_AUTO,
	O3_RX_REASON_PLOAMU_CTRL_FORCE,
	O3_RX_REASON_PLOAMU_CTRL_INVARIANT,
	O3_RX_REASON_PLOAMU_CTRL_RESTORE,
	O3_RX_REASON_FORMATTER_WRITE,
	O3_RX_REASON_O3_ACTIVATION,
	O3_RX_REASON_O3_GUARD,
	O3_RX_REASON_O3_FIFO_PARTIAL,
	O3_RX_REASON_O3_RECORD_LIMIT,
	O3_RX_REASON_O3_POLL_LIMIT,
	O3_RX_REASON_O3_POLL_GAP,
	O3_RX_REASON_O3_FIFO_OVERRUN,
	O3_RX_REASON_O3_EXT_BURST_WRITE,
	O3_RX_REASON_O3_OBSERVATION_COMPLETE,
	O3_RX_REASON_CLEANUP_BOUNDARY_CHANGED,
	O3_RX_REASON_FORMATTER_RESTORE,
};

enum o3_rx_record_class {
	O3_RX_RECORD_NONE,
	O3_RX_RECORD_UPSTREAM_OVERHEAD,
	O3_RX_RECORD_EXT_BURST_LENGTH,
	O3_RX_RECORD_OTHER_BROADCAST,
	O3_RX_RECORD_OTHER,
};

enum o3_rx_cleanup_class {
	O3_RX_CLEANUP_NONE,
	O3_RX_CLEANUP_TX_BURST_PLUS_ONE,
	O3_RX_CLEANUP_TX_BURST_PLUS_ONE_PLOAMU_UNDERRUN,
};

struct o3_rx_formatter_snapshot {
	u32 mac_guard;
	u32 mac_preamble12;
	u32 mac_preamble3;
	u32 mac_pre_delay;
	u32 phy_preamble;
	u32 phy_delimiter_guard;
	u32 phy_ext_preamble;
};

struct o3_rx_upstream_overhead {
	u8 guard_bits;
	u8 preamble1;
	u8 preamble2;
	u8 preamble3_pattern;
	u8 delimiter[3];
	bool delay_mode;
	u16 delay_time;
};

struct xpon_guard_snapshot {
	u32 physet2;
	u32 physet3;
	u32 physet10;
	u32 physta1;
	u32 setting;
	u32 misc;
	u32 phyrx_status;
	u32 prbs_tx;
	u32 test_frame;
	u32 trans_status;
	u32 int_enable;
	u32 int_status;
	int tx_disable_raw;
};

/*
 * These PHY fields are deliberately raw and unlatched.  The OEM counter-latch
 * trigger is never read or written by this RX-only module.  Both changed and
 * unchanged values are diagnostic and non-decisional; in particular, an
 * unchanged raw value is explicitly non-conclusive.
 */
struct o3_rx_tx_correlation_snapshot {
	u32 mac_tx_gem_count;
	u32 phy_tx_status_raw_unlatched;
	u32 phy_tx_frame_count_raw_unlatched;
	u32 phy_tx_burst_count_raw_unlatched;
};

struct gpon_mac_snapshot {
	u64 elapsed_ns;
	u32 onu_id;
	u32 global_config;
	u32 vendor_id;
	u32 vs_sn;
	u32 sn_msg_cfg;
	u32 interrupt_status;
	u32 interrupt_enable;
	u32 ploamu_fifo_status;
	u32 ploamd_fifo_status;
	u32 activation_status;
	u32 response_time;
	u32 mbi_stop;
	u32 dbg_cap_setting;
	u32 dbg_delay;
	u32 dbg_idle_gem_threshold;
	u32 dbg_ploamd_filter_in_o5;
	u32 o3_o4_ploamu_control;
	u32 rx_gem_count;
	u32 rx_crc_error_count;
	u32 rx_gtc_count;
	u32 tx_burst_count;
	struct o3_rx_tx_correlation_snapshot tx_correlation;
	bool tx_gem_valid;
	bool tx_correlation_valid;
	u32 rx_hec_one_error_count;
	u32 rx_hec_two_error_count;
	u32 rx_hec_uncorrectable_count;
	u32 ds_spf_count;
};

struct o3_rx_result {
	struct xpon_guard_snapshot guard_before;
	struct xpon_guard_snapshot guard_pre_pop;
	struct xpon_guard_snapshot guard_after_first;
	struct xpon_guard_snapshot guard_post_pop;
	struct xpon_guard_snapshot guard_after;
	struct gpon_mac_snapshot mac_before;
	struct gpon_mac_snapshot mac_after;
	struct gpon_mac_snapshot fifo_after_first;
	struct gpon_mac_snapshot fifo_post;
	struct gpon_mac_snapshot first_gtc;
	struct gpon_mac_snapshot stage_a_readback;
	struct gpon_mac_snapshot stage_a_end;
	struct gpon_mac_snapshot stage_b_readback;
	struct gpon_mac_snapshot first_eof_reassert;
	struct gpon_mac_snapshot observation_end;
	struct gpon_mac_snapshot downstream_trigger;
	struct gpon_mac_snapshot unsafe_status;
	struct gpon_mac_snapshot o3_final;
	struct xpon_guard_snapshot o3_guard_final;
	struct o3_rx_formatter_snapshot formatter_before;
	struct o3_rx_formatter_snapshot formatter_programmed;
	struct o3_rx_formatter_snapshot formatter_after;
	struct o3_rx_tx_correlation_snapshot o3_tx_correlation_last;
	struct o3_rx_tx_correlation_snapshot cleanup_tx_correlation_last;
	unsigned int first_gtc_polls;
	unsigned int passive_trigger_polls;
	unsigned int stage_a_polls;
	unsigned int stage_b_polls;
	unsigned int checks;
	unsigned int gpon_reads;
	unsigned int gpon_writes;
	unsigned int fifo_data_reads;
	unsigned int irq_status_writes;
	unsigned int irq_enable_writes;
	unsigned int identity_writes;
	unsigned int upstream_fifo_writes;
	unsigned int gpio_pinctrl_writes;
	unsigned int non_formatter_xpon_writes;
	unsigned int phy_laser_apd_en7570_writes;
	unsigned int scu_update_calls;
	u32 wan_before;
	u32 wan_gpon;
	u32 wan_after;
	u32 wan_mismatch;
	u32 wan_pre_pop;
	u32 wan_post_pop;
	u32 reset_ctrl2;
	u32 reset_ctrl1;
	u32 activation_before;
	u32 activation_o2;
	u32 activation_after;
	u32 stage_a_pre_status;
	u32 stage_a_post_status;
	u32 stage_b_pre_status;
	u32 stage_b_post_status;
	u32 baseline_status;
	u32 baseline_unsafe_mask;
	u32 baseline_rx_gtc;
	u32 startup_status;
	u32 startup_unsafe_mask;
	u32 stage_a_non_target_lost;
	u32 stage_b_non_target_lost;
	u32 unsafe_interrupt_mask;
	u32 downstream_trigger_source;
	u32 accepted_trigger_status;
	u32 final_pre_pop_status;
	u32 final_pre_pop_fifo_status;
	u32 second_pre_pop_status;
	u32 second_pre_pop_fifo_status;
	u32 ploamd_words[O3_RX_MAX_RECORDS]
			[O3_RX_PLOAMD_WORDS_PER_RECORD];
	enum o3_rx_record_class record_class[O3_RX_MAX_RECORDS];
	u32 o3_interrupt_seen;
	u32 activation_o3;
	u32 identity_changed_mask;
	u32 sn_msg_cfg_changed_fields;
	u32 activation_o3_guard_mask;
	u32 ploamu_control_before;
	u32 ploamu_control_forced;
	u32 ploamu_control_program_readback;
	u32 ploamu_control_after;
	u32 cleanup_tx_burst_latched_value;
	u32 cleanup_tx_burst_guard_last;
	u32 cleanup_tx_burst_latch_reject_mask;
	u32 cleanup_ploamu_accepted_status;
	u32 cleanup_ploamu_accepted_delta;
	u32 cleanup_ploamu_guard_last;
	u32 cleanup_ploamd_accepted_status;
	u32 cleanup_ploamd_guard_last;
	u32 cleanup_irq_unsafe_last;
	u32 cleanup_irq_unsafe_mask;
	u32 upstream_activity_latch_sources;
	unsigned int record_count;
	unsigned int upstream_overhead_count;
	unsigned int extended_burst_count;
	unsigned int extended_burst_formatter_writes;
	unsigned int other_record_count;
	unsigned int max_records_used;
	unsigned int observe_ms_used;
	unsigned int xpon_formatter_writes;
	unsigned int ploamu_control_writes;
	unsigned int restore_write_attempts;
	unsigned int restore_guard_checks;
	unsigned int restore_guard_failures;
	unsigned int restore_readback_failures;
	unsigned int cleanup_tx_burst_latch_attempts;
	unsigned int cleanup_tx_burst_guard_checks;
	unsigned int cleanup_tx_burst_guard_failures;
	unsigned int mac_tx_gem_samples;
	unsigned int tx_correlation_samples;
	unsigned int cleanup_tx_correlation_samples;
	unsigned int phy_tx_correlation_reads;
	unsigned int o3_poll_count;
	u64 hold_ns;
	u64 cycle_ns;
	u64 fifo_pop_ns;
	u64 first_pop_ns;
	u64 second_pop_ns;
	u64 trigger_to_o1_ns;
	u64 trigger_to_post_ns;
	u64 irq_off_ns;
	u64 first_gtc_elapsed_ns;
	u64 stage_a_clear_elapsed_ns;
	u64 stage_a_readback_latency_ns;
	u64 stage_a_end_elapsed_ns;
	u64 stage_b_clear_elapsed_ns;
	u64 stage_b_readback_latency_ns;
	u64 first_eof_reassert_latency_ns;
	u64 last_check_elapsed_ns;
	u64 max_check_gap_ns;
	u64 terminal_gap_ns;
	u64 o3_last_poll_ns;
	u64 o3_max_poll_gap_ns;
	int sequence_result;
	int o2_limit_result;
	int activation_restore_result;
	int mac_after_result;
	int trigger_gate_result;
	int pre_pop_result;
	int guard_pre_pop_result;
	int guard_post_pop_result;
	int fifo_pop_limit_result;
	int first_pop_post_result;
	int guard_after_first_result;
	int second_pre_pop_result;
	int fifo_post_result;
	int post_pop_mux_result;
	int terminal_gap_result;
	int restore_result;
	int final_read_result;
	int guard_after_result;
	int initial_records_result;
	int formatter_program_result;
	int activation_o3_result;
	int activation_o3_guard_result;
	int o3_observation_result;
	int formatter_restore_result;
	int ploamu_control_program_result;
	int ploamu_control_restore_pre_guard_result;
	int ploamu_control_restore_result;
	int ploamu_control_restore_post_guard_result;
	int o1_restore_pre_guard_result;
	int o1_restore_post_guard_result;
	int restore_mode_guard_result;
	enum o3_rx_stop_reason stop_reason;
	enum o3_rx_cleanup_class cleanup_class;
	bool update_attempted;
	bool activation_write_attempted;
	bool stage_a_passive_sampled;
	bool stage_b_passive_sampled;
	bool first_gtc_valid;
	bool stage_a_readback_valid;
	bool stage_a_end_valid;
	bool stage_b_readback_valid;
	bool first_eof_reassert_valid;
	bool observation_end_valid;
	bool downstream_trigger_valid;
	bool final_pre_pop_valid;
	bool fifo_after_first_valid;
	bool guard_after_first_valid;
	bool second_pre_pop_valid;
	bool unsafe_status_valid;
	bool fifo_pop_armed;
	bool fifo_pop_attempted;
	bool fifo_pop_completed;
	bool first_record_captured;
	bool second_record_captured;
	bool records_valid;
	bool initial_records_identical;
	bool third_record_matches;
	bool formatter_before_valid;
	bool formatter_program_attempted;
	bool formatter_programmed_valid;
	bool formatter_restored_valid;
	bool activation_o3_attempted;
	bool activation_o3_valid;
	bool o3_observation_started;
	bool o3_final_valid;
	bool o3_guard_final_valid;
	bool sn_request_seen;
	bool sn_internal_send_seen;
	bool extended_burst_seen;
	bool o3_tx_burst_changed;
	bool o3_ploamu_status_changed;
	u32 o3_tx_burst_last;
	u32 o3_ploamu_status_last;
	bool preflight_passed;
	bool guard_pre_pop_valid;
	bool guard_post_pop_valid;
	bool guard_after_valid;
	bool fifo_post_valid;
	bool wan_post_pop_valid;
	bool baseline_expected_match;
	bool baseline_eof_preexisting;
	bool startup_expected_match;
	bool identity_snapshot_unchanged;
	bool onu_id_never_valid;
	bool sn_msg_cfg_rdm_dly_only_allowed;
	bool sn_msg_cfg_rdm_dly_residual;
	bool cold_power_cycle_required;
	bool ploamu_control_force_requested;
	bool ploamu_control_baseline_hw_auto;
	bool ploamu_control_force_attempted;
	bool ploamu_control_forced_valid;
	bool ploamu_control_restore_attempted;
	bool ploamu_control_restored_valid;
	bool ploamu_control_restore_skipped_no_o1;
	bool ploamu_control_restore_skipped_guard;
	bool ploamu_control_restore_skipped_upstream_latch;
	bool ploamu_control_restore_skipped_formatter;
	bool cleanup_tx_burst_latched;
	bool cleanup_tx_burst_changed_again;
	bool cleanup_ploamu_changed_again;
	bool cleanup_ploamd_changed_again;
	bool cleanup_irq_changed_again;
	bool upstream_activity_latched;
	bool o3_tx_correlation_valid;
	bool phy_tx_status_raw_changed_seen;
	bool phy_tx_frame_raw_changed_seen;
	bool phy_tx_burst_raw_changed_seen;
	bool cleanup_tx_correlation_valid;
	bool stage_b_expected_match;
	bool mac_before_valid;
	bool mac_after_valid;
	bool o1_restore_pre_guard_valid;
	bool o1_restore_post_guard_valid;
	bool restore_mode_guard_valid;
	bool restore_unsafe;
	bool unsafe_pinned;
	struct dentry *debugfs_dir;
};

struct o3_rx_context {
	struct regmap *scu;
	void __iomem *xpon;
	void __iomem *gpon;
	struct gpio_desc *tx_disable;
};

static bool arm_o3_rx_lab;
module_param(arm_o3_rx_lab, bool, 0400);
MODULE_PARM_DESC(arm_o3_rx_lab,
		 "Arm one reversible downstream-only GPON O2-to-O3 lab transaction");

static bool force_software_ploamu_control;
module_param(force_software_ploamu_control, bool, 0400);
MODULE_PARM_DESC(force_software_ploamu_control,
		 "Opt in to forcing O3/O4 PLOAMu control from hardware-auto to software");

static unsigned int max_records = O3_RX_DEFAULT_MAX_RECORDS;
module_param(max_records, uint, 0400);
MODULE_PARM_DESC(max_records,
		 "Maximum downstream records retained, including the two O2 records");

static unsigned int observe_ms = O3_RX_DEFAULT_OBSERVE_MS;
module_param(observe_ms, uint, 0400);
MODULE_PARM_DESC(observe_ms, "Bounded local-O3 observation time in milliseconds");

static struct o3_rx_result result;
static struct o3_rx_context o3_rx_ctx;
static bool o3_rx_hw_resources;

static void capture_gpon_probe(struct o3_rx_context *ctx,
			       struct gpon_mac_snapshot *s, u64 start_ns);
static void capture_gpon_checkpoint(struct o3_rx_context *ctx,
				    struct gpon_mac_snapshot *s, u64 start_ns);
static int verify_phase28_binding(void);

static u32 o3_rx_read_twice_second(void __iomem *address)
{
	(void)ioread32(address);
	return ioread32(address);
}

static void o3_rx_capture_mac_tx_gem(
	struct o3_rx_context *ctx,
	struct o3_rx_tx_correlation_snapshot *snapshot)
{
	snapshot->mac_tx_gem_count =
		ioread32(ctx->gpon + GPON_DBG_TX_GEM_CNT);
	result.gpon_reads++;
	result.mac_tx_gem_samples++;
}

static void o3_rx_capture_tx_correlation(
	struct o3_rx_context *ctx,
	struct o3_rx_tx_correlation_snapshot *snapshot)
{
	o3_rx_capture_mac_tx_gem(ctx, snapshot);
	/*
	 * These are raw, unlatched diagnostics.  Read each register twice and
	 * retain the second value, but never touch the OEM latch trigger.
	 */
	snapshot->phy_tx_status_raw_unlatched =
		o3_rx_read_twice_second(ctx->xpon + XPON_PHY_TX_STATUS_RAW);
	snapshot->phy_tx_frame_count_raw_unlatched =
		o3_rx_read_twice_second(
			ctx->xpon + XPON_PHY_TX_FRAME_COUNTER_RAW);
	snapshot->phy_tx_burst_count_raw_unlatched =
		o3_rx_read_twice_second(
			ctx->xpon + XPON_PHY_TX_BURST_COUNTER_RAW);
	result.phy_tx_correlation_reads += 6;
	result.tx_correlation_samples++;
}

static u32 o3_rx_tx_correlation_upstream_sources(
	const struct o3_rx_tx_correlation_snapshot *snapshot)
{
	if (snapshot->mac_tx_gem_count !=
	    result.mac_before.tx_correlation.mac_tx_gem_count)
		return O3_RX_UPSTREAM_LATCH_TX_GEM;

	return 0;
}

static void o3_rx_note_phy_raw_diagnostics(
	const struct o3_rx_tx_correlation_snapshot *snapshot)
{
	const struct o3_rx_tx_correlation_snapshot *baseline =
		&result.mac_before.tx_correlation;

	result.phy_tx_status_raw_changed_seen |=
		snapshot->phy_tx_status_raw_unlatched !=
		baseline->phy_tx_status_raw_unlatched;
	result.phy_tx_frame_raw_changed_seen |=
		snapshot->phy_tx_frame_count_raw_unlatched !=
		baseline->phy_tx_frame_count_raw_unlatched;
	result.phy_tx_burst_raw_changed_seen |=
		snapshot->phy_tx_burst_count_raw_unlatched !=
		baseline->phy_tx_burst_count_raw_unlatched;
}

static void o3_rx_record_cleanup_tx_correlation(
	const struct o3_rx_tx_correlation_snapshot *snapshot)
{
	result.cleanup_tx_correlation_last = *snapshot;
	result.cleanup_tx_correlation_valid = true;
	result.cleanup_tx_correlation_samples++;
	o3_rx_note_phy_raw_diagnostics(snapshot);
}

static void o3_rx_set_upstream_stop_reason(u32 sources)
{
	if (sources & O3_RX_UPSTREAM_LATCH_TX_STATUS)
		result.stop_reason = O3_RX_REASON_TX_ACTIVITY;
	else if (sources & O3_RX_UPSTREAM_LATCH_TX_GEM)
		result.stop_reason = O3_RX_REASON_TX_GEM_CHANGED;
	else if (result.cleanup_class ==
		 O3_RX_CLEANUP_TX_BURST_PLUS_ONE_PLOAMU_UNDERRUN &&
		 sources == (O3_RX_UPSTREAM_LATCH_TX_BURST |
			     O3_RX_UPSTREAM_LATCH_PLOAMU))
		result.stop_reason =
			O3_RX_REASON_TX_BURST_PLOAMU_UNDERRUN;
	else if (sources & O3_RX_UPSTREAM_LATCH_TX_BURST)
		result.stop_reason = O3_RX_REASON_TX_BURST_CHANGED;
	else
		result.stop_reason = O3_RX_REASON_PLOAMU_CHANGED;
}

static bool o3_rx_baseline_status_allowed(u32 status)
{
	return status == O3_RX_BASELINE_PRE_DS_FEC ||
	       status == O3_RX_BASELINE_NO_EOF ||
	       status == O3_RX_BASELINE_EOF_LATCHED;
}

enum o3_rx_worker_state {
	O3_RX_WORKER_NOT_STARTED,
	O3_RX_WORKER_RUNNING,
	O3_RX_WORKER_DONE,
	O3_RX_WORKER_UNSAFE_PINNED,
};

static int o3_rx_worker_state;

static void o3_rx_publish_worker_state(int state)
{
	/* Publish completed result/resource updates before exposing the state. */
	smp_store_release(&o3_rx_worker_state, state);
}

static u8 o3_rx_record_byte(const u32 words[O3_RX_PLOAMD_WORDS_PER_RECORD],
			    unsigned int byte)
{
	unsigned int word = byte / sizeof(u32);
	unsigned int shift = (3 - byte % sizeof(u32)) * 8;

	return (words[word] >> shift) & 0xff;
}

static enum o3_rx_record_class
o3_rx_classify_record(const u32 words[O3_RX_PLOAMD_WORDS_PER_RECORD])
{
	u8 destination = o3_rx_record_byte(words, 0);
	u8 type = o3_rx_record_byte(words, 1);

	if (destination == O3_RX_PLOAM_BROADCAST &&
	    type == O3_RX_PLOAM_UPSTREAM_OVERHEAD)
		return O3_RX_RECORD_UPSTREAM_OVERHEAD;
	if (destination == O3_RX_PLOAM_BROADCAST &&
	    type == O3_RX_PLOAM_EXT_BURST_LENGTH)
		return O3_RX_RECORD_EXT_BURST_LENGTH;
	if (destination == O3_RX_PLOAM_BROADCAST)
		return O3_RX_RECORD_OTHER_BROADCAST;

	return O3_RX_RECORD_OTHER;
}

static void o3_rx_count_record(unsigned int index)
{
	switch (result.record_class[index]) {
	case O3_RX_RECORD_UPSTREAM_OVERHEAD:
		result.upstream_overhead_count++;
		break;
	case O3_RX_RECORD_EXT_BURST_LENGTH:
		result.extended_burst_count++;
		result.extended_burst_seen = true;
		break;
	case O3_RX_RECORD_OTHER_BROADCAST:
	case O3_RX_RECORD_OTHER:
		result.other_record_count++;
		break;
	case O3_RX_RECORD_NONE:
		break;
	}
}

static void o3_rx_parse_upstream_overhead(
	const u32 words[O3_RX_PLOAMD_WORDS_PER_RECORD],
	struct o3_rx_upstream_overhead *overhead)
{
	overhead->guard_bits = o3_rx_record_byte(words, 2);
	overhead->preamble1 = o3_rx_record_byte(words, 3);
	overhead->preamble2 = o3_rx_record_byte(words, 4);
	overhead->preamble3_pattern = o3_rx_record_byte(words, 5);
	overhead->delimiter[0] = o3_rx_record_byte(words, 6);
	overhead->delimiter[1] = o3_rx_record_byte(words, 7);
	overhead->delimiter[2] = o3_rx_record_byte(words, 8);
	overhead->delay_mode = !!(o3_rx_record_byte(words, 9) & BIT(5));
	overhead->delay_time = (u16)o3_rx_record_byte(words, 10) << 8 |
			       o3_rx_record_byte(words, 11);
}

static void o3_rx_capture_formatter(struct o3_rx_context *ctx,
				    struct o3_rx_formatter_snapshot *snapshot)
{
	snapshot->mac_guard =
		ioread32(ctx->gpon + GPON_G_PLOU_GUARD_BIT);
	snapshot->mac_preamble12 =
		ioread32(ctx->gpon + GPON_G_PLOU_PREAMBLE12);
	snapshot->mac_preamble3 =
		ioread32(ctx->gpon + GPON_G_PLOU_PREAMBLE3);
	snapshot->mac_pre_delay =
		ioread32(ctx->gpon + GPON_G_PRE_ASSIGNED_DELAY);
	snapshot->phy_preamble =
		ioread32(ctx->xpon + XPON_GPON_PREAMBLE);
	snapshot->phy_delimiter_guard =
		ioread32(ctx->xpon + XPON_GPON_DELIMITER_GUARD);
	snapshot->phy_ext_preamble =
		ioread32(ctx->xpon + XPON_GPON_EXT_PREAMBLE);
	result.gpon_reads += 4;
}

static bool o3_rx_formatter_equal(const struct o3_rx_formatter_snapshot *a,
				  const struct o3_rx_formatter_snapshot *b)
{
	return a->mac_guard == b->mac_guard &&
	       a->mac_preamble12 == b->mac_preamble12 &&
	       a->mac_preamble3 == b->mac_preamble3 &&
	       a->mac_pre_delay == b->mac_pre_delay &&
	       a->phy_preamble == b->phy_preamble &&
	       a->phy_delimiter_guard == b->phy_delimiter_guard &&
	       a->phy_ext_preamble == b->phy_ext_preamble;
}

/* Never expose factory identity words; only retain their equality result. */
static void o3_rx_note_sn_msg_cfg_delta(u32 sn_msg_cfg)
{
	u32 diff = sn_msg_cfg ^ result.mac_before.sn_msg_cfg;
	u32 fields = 0;

	if (diff & O3_RX_SN_MSG_THRESHOLD)
		fields |= O3_RX_SN_MSG_CHANGE_THRESHOLD;
	if (diff & O3_RX_SN_MSG_TX_POWER)
		fields |= O3_RX_SN_MSG_CHANGE_TX_POWER;
	if (diff & O3_RX_SN_MSG_RANDOM_DELAY)
		fields |= O3_RX_SN_MSG_CHANGE_RANDOM_DELAY;
	if (diff & ~O3_RX_SN_MSG_KNOWN)
		fields |= O3_RX_SN_MSG_CHANGE_RESERVED;
	result.sn_msg_cfg_changed_fields |= fields;
}

/*
 * Experimental exception from repeated cold-boot RX-only observations: only
 * the random-delay field may vary after an exact local-O3 readback.
 * Threshold, TX-power mode and reserved bits remain strict.  This function
 * never writes G_SN_MSG_CFG and does not claim that the MAC owns the field.
 */
static bool o3_rx_sn_msg_cfg_matches(u32 sn_msg_cfg, bool allow_rdm_dly)
{
	u32 diff = sn_msg_cfg ^ result.mac_before.sn_msg_cfg;

	if (!diff)
		return true;
	o3_rx_note_sn_msg_cfg_delta(sn_msg_cfg);
	if (!allow_rdm_dly || diff & ~O3_RX_SN_MSG_RANDOM_DELAY)
		return false;
	result.sn_msg_cfg_rdm_dly_only_allowed = true;

	return true;
}

/*
 * A config delta which remains after local O1 restoration is not treated as
 * an exact baseline restore.  This must run while the WAN mux still selects
 * GPON: after the mux is restored to ATM, the GPON MMIO window is no longer
 * safe to read.  Retain resources and require a physical power cycle rather
 * than writing the value back.
 */
static void o3_rx_note_final_sn_msg_cfg(struct o3_rx_context *ctx)
{
	u32 sn_msg_cfg = ioread32(ctx->gpon + GPON_G_SN_MSG_CFG);
	u32 diff = sn_msg_cfg ^ result.mac_before.sn_msg_cfg;

	result.gpon_reads++;
	if (!diff)
		return;
	o3_rx_note_sn_msg_cfg_delta(sn_msg_cfg);
	result.cold_power_cycle_required = true;
	if (!(diff & ~O3_RX_SN_MSG_RANDOM_DELAY))
		result.sn_msg_cfg_rdm_dly_residual = true;
	result.restore_unsafe = true;
}

static bool o3_rx_identity_matches(u32 onu_id, u32 vendor_id, u32 vs_sn)
{
	u32 changed = 0;

	if (onu_id & ONU_ID_VALID)
		result.onu_id_never_valid = false;
	if (onu_id != result.mac_before.onu_id)
		changed |= O3_RX_ID_CHANGE_ONU_ID;
	if (vendor_id != result.mac_before.vendor_id)
		changed |= O3_RX_ID_CHANGE_VENDOR_ID;
	if (vs_sn != result.mac_before.vs_sn)
		changed |= O3_RX_ID_CHANGE_VS_SN;
	if (changed) {
		result.identity_changed_mask |= changed;
		result.identity_snapshot_unchanged = false;
	}

	return !changed;
}

static bool o3_rx_snapshot_matches(const struct gpon_mac_snapshot *mac,
				   bool allow_rdm_dly)
{
	bool identity_ok;
	bool sn_msg_cfg_ok;

	identity_ok = o3_rx_identity_matches(mac->onu_id, mac->vendor_id,
					     mac->vs_sn);
	sn_msg_cfg_ok = o3_rx_sn_msg_cfg_matches(mac->sn_msg_cfg,
						     allow_rdm_dly);

	return identity_ok && sn_msg_cfg_ok;
}

static u32 o3_rx_expected_ploamu_control(void)
{
	if (result.ploamu_control_forced_valid)
		return result.ploamu_control_forced;

	return result.ploamu_control_before;
}

static bool o3_rx_ploamu_control_matches(u32 value)
{
	return value == o3_rx_expected_ploamu_control();
}

static void capture_xpon_guard(struct o3_rx_context *ctx,
			       struct xpon_guard_snapshot *s)
{
	s->tx_disable_raw = gpiod_get_raw_value(ctx->tx_disable);
	s->physet2 = ioread32(ctx->xpon + XPON_PHYSET2);
	s->physet3 = ioread32(ctx->xpon + XPON_PHYSET3);
	s->physet10 = ioread32(ctx->xpon + XPON_PHYSET10);
	s->physta1 = ioread32(ctx->xpon + XPON_PHYSTA1);
	s->setting = ioread32(ctx->xpon + XPON_SETTING);
	s->misc = ioread32(ctx->xpon + XPON_MISC);
	s->phyrx_status = ioread32(ctx->xpon + XPON_PHYRX_STATUS);
	s->prbs_tx = ioread32(ctx->xpon + XPON_BISTCTL_PRBS_TX_EN);
	s->test_frame = ioread32(ctx->xpon + XPON_TEST_FRAME_EN);
	s->trans_status = ioread32(ctx->xpon + XPON_TRANS_STATUS);
	s->int_enable = ioread32(ctx->xpon + XPON_INT_ENABLE);
	s->int_status = ioread32(ctx->xpon + XPON_INT_STATUS);
}

static int verify_xpon_guard(const struct xpon_guard_snapshot *s)
{
	if (s->tx_disable_raw != 1)
		return -EPERM;
	if ((s->physet2 & ~PHYSET2_STATUS_BIT) !=
	    (0x00003c00 | PHYSET2_FW_READY))
		return -EUCLEAN;
	if (s->physet3 != 0x4581e110 ||
	    s->physet3 & PHYSET3_CONTINUOUS_MODE)
		return -EUCLEAN;
	if (!(s->physet10 & PHYSET10_GPON_MODE) ||
	    FIELD_GET(PHYSTA1_STATE, s->physta1) != PHYSTA1_READY ||
	    s->setting != XPON_SETTING_EN7570)
		return -ENOLINK;
	if (s->misc & XPON_MISC_ROGUE_TX || s->prbs_tx || s->test_frame ||
	    s->int_enable)
		return -EPERM;
	if (FIELD_GET(PHYRX_SYNC, s->phyrx_status) != PHYRX_SYNC_GPON ||
	    !(s->phyrx_status & PHYRX_FEC) ||
	    s->trans_status & TRANS_STATUS_LOS)
		return -ENOLINK;

	return 0;
}

/*
 * This is the minimum pre/post boundary oracle for writes which occur before
 * a GPON-MAC snapshot exists, or during restoration where the write must still
 * be attempted after a failed check.  It proves the Phase28 owner remains
 * bound, GPIO16 TX_DISABLE remains asserted, and the audited xPON invariants
 * remain unchanged.  PHYSET3 bit 5 is an invariant, not a second TX kill.
 */
static int o3_rx_phase28_xpon_guard(struct o3_rx_context *ctx,
				     struct xpon_guard_snapshot *snapshot)
{
	struct xpon_guard_snapshot local;
	struct xpon_guard_snapshot *guard = snapshot ?: &local;
	int err;

	capture_xpon_guard(ctx, guard);
	err = verify_xpon_guard(guard);
	if (err)
		return err;

	return verify_phase28_binding();
}

static void o3_rx_latch_upstream_activity(u32 sources)
{
	if (!sources)
		return;

	result.upstream_activity_latched = true;
	result.upstream_activity_latch_sources |= sources;
	result.cold_power_cycle_required = true;
	result.restore_unsafe = true;
}

static u32 o3_rx_cleanup_class_sources(enum o3_rx_cleanup_class class)
{
	switch (class) {
	case O3_RX_CLEANUP_TX_BURST_PLUS_ONE:
		return O3_RX_UPSTREAM_LATCH_TX_BURST;
	case O3_RX_CLEANUP_TX_BURST_PLUS_ONE_PLOAMU_UNDERRUN:
		return O3_RX_UPSTREAM_LATCH_TX_BURST |
		       O3_RX_UPSTREAM_LATCH_PLOAMU;
	case O3_RX_CLEANUP_NONE:
		return 0;
	}

	return 0;
}

static bool o3_rx_cleanup_tx_burst_eligible(void)
{
	return result.cleanup_tx_burst_latched &&
	       !result.cleanup_tx_burst_changed_again &&
	       !result.cleanup_ploamu_changed_again &&
	       !result.cleanup_ploamd_changed_again &&
	       !result.cleanup_irq_changed_again &&
	       !result.cleanup_irq_unsafe_mask &&
	       result.cleanup_tx_burst_latched_value ==
		       result.mac_before.tx_burst_count + 1 &&
	       !(result.cleanup_ploamd_accepted_status &
		 PLOAMD_FIFO_OVERRUN) &&
	       !(FIELD_GET(PLOAM_FIFO_LEVEL,
			   result.cleanup_ploamd_accepted_status) %
		 O3_RX_PLOAMD_WORDS_PER_RECORD) &&
	       result.cleanup_class != O3_RX_CLEANUP_NONE &&
	       result.cleanup_ploamu_accepted_status ==
		       (result.mac_before.ploamu_fifo_status |
			result.cleanup_ploamu_accepted_delta) &&
	       (result.cleanup_ploamu_accepted_delta == 0 ||
		(result.cleanup_class ==
			 O3_RX_CLEANUP_TX_BURST_PLUS_ONE_PLOAMU_UNDERRUN &&
		 !(result.mac_before.ploamu_fifo_status &
		   PLOAMU_FIFO_UNDERRUN) &&
		 result.cleanup_ploamu_accepted_delta ==
		   PLOAMU_FIFO_UNDERRUN)) &&
	       result.upstream_activity_latch_sources ==
		       o3_rx_cleanup_class_sources(result.cleanup_class);
}

static int o3_rx_guard_common_expected(struct o3_rx_context *ctx,
				       unsigned int expected_state,
				       bool strict_internal,
				       bool check_ploamu_control,
				       bool cleanup_guard)
{
	struct xpon_guard_snapshot guard;
	struct o3_rx_tx_correlation_snapshot tx_correlation;
	u32 interrupt_status;
	u32 ploamu_control;
	u32 ploamu_fifo_status;
	u32 ploamd_fifo_status;
	u32 tx_burst_count;
	u32 expected_tx_burst;
	u32 expected_ploamu_status;
	u32 upstream_sources = 0;
	u32 value;
	bool identity_ok;
	bool sn_msg_cfg_ok;
	bool allow_rdm_dly;
	bool tx_quiet_required;
	bool cleanup_snapshot;
	bool cleanup_boundary_failed = false;
	int err;

	expected_tx_burst = result.mac_before.tx_burst_count;
	expected_ploamu_status = result.mac_before.ploamu_fifo_status;
	if (cleanup_guard && result.cleanup_tx_burst_latched) {
		expected_tx_burst = result.cleanup_tx_burst_latched_value;
		expected_ploamu_status =
			result.cleanup_ploamu_accepted_status;
		result.cleanup_tx_burst_guard_checks++;
	}
	err = o3_rx_phase28_xpon_guard(ctx, &guard);
	if (err)
		return err;
	if (FIELD_GET(ACTIVATION_STATE,
		      ioread32(ctx->gpon + GPON_G_ACTIVATION_ST)) !=
	    expected_state)
		return -EPROTO;
	if (ioread32(ctx->gpon + GPON_G_INT_ENABLE))
		return -EPERM;
	tx_quiet_required = strict_internal ||
			    expected_state == ACTIVATION_O3 ||
			    result.ploamu_control_force_attempted;
	cleanup_snapshot = cleanup_guard ||
			   (expected_state == ACTIVATION_O1 &&
			    result.activation_o3_attempted);
	if (tx_quiet_required) {
		interrupt_status = ioread32(ctx->gpon + GPON_G_INT_STATUS);
		if (cleanup_guard && result.cleanup_tx_burst_latched) {
			result.cleanup_irq_unsafe_last =
				interrupt_status & ~O3_RX_O3_ALLOWED_STATUS;
			result.cleanup_irq_unsafe_mask |=
				result.cleanup_irq_unsafe_last;
			if (result.cleanup_irq_unsafe_last) {
				result.cleanup_tx_burst_guard_failures++;
				result.cleanup_irq_changed_again = true;
				cleanup_boundary_failed = true;
			}
		}
		if (interrupt_status & INT_TX_ACTIVITY)
			upstream_sources |= O3_RX_UPSTREAM_LATCH_TX_STATUS;
		tx_burst_count =
			ioread32(ctx->gpon + GPON_DBG_TX_BST_CNT);
		if (cleanup_guard && result.cleanup_tx_burst_latched)
			result.cleanup_tx_burst_guard_last = tx_burst_count;
		if (tx_burst_count != expected_tx_burst) {
			upstream_sources |= O3_RX_UPSTREAM_LATCH_TX_BURST;
			if (cleanup_guard && result.cleanup_tx_burst_latched) {
				result.cleanup_tx_burst_guard_failures++;
				result.cleanup_tx_burst_changed_again = true;
			}
		}
		ploamu_fifo_status =
			ioread32(ctx->gpon + GPON_G_PLOAMU_FIFO_STS);
		if (cleanup_guard && result.cleanup_tx_burst_latched)
			result.cleanup_ploamu_guard_last = ploamu_fifo_status;
		if (ploamu_fifo_status != expected_ploamu_status) {
			upstream_sources |= O3_RX_UPSTREAM_LATCH_PLOAMU;
			if (cleanup_guard && result.cleanup_tx_burst_latched) {
				result.cleanup_tx_burst_guard_failures++;
				result.cleanup_ploamu_changed_again = true;
			}
		}
		if (cleanup_guard && result.cleanup_tx_burst_latched) {
			ploamd_fifo_status =
				ioread32(ctx->gpon + GPON_G_PLOAMD_FIFO_STS);
			result.gpon_reads++;
			result.cleanup_ploamd_guard_last = ploamd_fifo_status;
			if (ploamd_fifo_status !=
			    result.cleanup_ploamd_accepted_status) {
				result.cleanup_tx_burst_guard_failures++;
				result.cleanup_ploamd_changed_again = true;
				cleanup_boundary_failed = true;
			}
		}
		o3_rx_capture_tx_correlation(ctx, &tx_correlation);
		upstream_sources |=
			o3_rx_tx_correlation_upstream_sources(&tx_correlation);
		if (cleanup_snapshot)
			o3_rx_record_cleanup_tx_correlation(&tx_correlation);
		else
			o3_rx_note_phy_raw_diagnostics(&tx_correlation);
		o3_rx_latch_upstream_activity(upstream_sources);
		if (upstream_sources || cleanup_boundary_failed) {
			if (upstream_sources)
				o3_rx_set_upstream_stop_reason(upstream_sources);
			else
				result.stop_reason =
					O3_RX_REASON_CLEANUP_BOUNDARY_CHANGED;
			return -EPERM;
		}
	}
	allow_rdm_dly = expected_state == ACTIVATION_O3 ||
			result.activation_o3_valid;
	identity_ok = o3_rx_identity_matches(
		ioread32(ctx->gpon + GPON_G_ONU_ID),
		ioread32(ctx->gpon + GPON_G_VENDOR_ID),
		ioread32(ctx->gpon + GPON_G_VS_SN));
	sn_msg_cfg_ok = o3_rx_sn_msg_cfg_matches(
		ioread32(ctx->gpon + GPON_G_SN_MSG_CFG), allow_rdm_dly);
	if (!identity_ok || !sn_msg_cfg_ok ||
	    ioread32(ctx->gpon + GPON_G_GBL_CFG) !=
	    result.mac_before.global_config)
		return -EUCLEAN;
	if (check_ploamu_control) {
		ploamu_control =
			ioread32(ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
		if (!o3_rx_ploamu_control_matches(ploamu_control))
			return -EUCLEAN;
	}
	err = regmap_read(ctx->scu, EN751221_SCU_WAN_CONF, &value);
	if (err)
		return err;
	if (value != result.wan_gpon)
		return -EUCLEAN;

	result.gpon_reads += tx_quiet_required ?
			      (check_ploamu_control ? 11 : 10) :
			      (check_ploamu_control ? 8 : 7);
	return 0;
}

static int o3_rx_guard_common_internal(struct o3_rx_context *ctx,
				       unsigned int expected_state,
				       bool strict_internal,
				       bool check_ploamu_control)
{
	return o3_rx_guard_common_expected(ctx, expected_state, strict_internal,
					   check_ploamu_control, false);
}

static int o3_rx_cleanup_guard_common_internal(struct o3_rx_context *ctx,
					       unsigned int expected_state,
					       bool check_ploamu_control)
{
	return o3_rx_guard_common_expected(ctx, expected_state, true,
					   check_ploamu_control, true);
}

static int o3_rx_guard_common(struct o3_rx_context *ctx,
			      unsigned int expected_state, bool strict_internal)
{
	return o3_rx_guard_common_internal(ctx, expected_state,
					   strict_internal, true);
}

/*
 * This helper is invoked only after a guard has already rejected the path.
 * It performs read-only classification so the next cold-boot experiment can
 * distinguish a physical failure from an autonomous MAC state change without
 * dumping serial/identity words or changing the original fail-closed policy.
 */
static u32 o3_rx_diagnose_guard(struct o3_rx_context *ctx,
				unsigned int expected_state,
				bool strict_internal)
{
	struct xpon_guard_snapshot guard;
	struct o3_rx_tx_correlation_snapshot tx_correlation;
	u32 mask = 0;
	u32 value;
	u32 ploamu_control;
	u32 ploamu_fifo_status;
	u32 interrupt_status;
	u32 tx_burst_count;
	u32 upstream_sources = 0;
	bool identity_ok;
	bool sn_msg_cfg_ok;
	bool allow_rdm_dly;
	bool tx_quiet_required;

	capture_xpon_guard(ctx, &guard);
	if (verify_xpon_guard(&guard))
		mask |= O3_RX_GUARD_DIAG_XPON;
	if (FIELD_GET(ACTIVATION_STATE,
		      ioread32(ctx->gpon + GPON_G_ACTIVATION_ST)) !=
	    expected_state)
		mask |= O3_RX_GUARD_DIAG_ACTIVATION;
	if (ioread32(ctx->gpon + GPON_G_INT_ENABLE))
		mask |= O3_RX_GUARD_DIAG_IRQ_ENABLE;
	tx_quiet_required = strict_internal ||
			    expected_state == ACTIVATION_O3 ||
			    result.ploamu_control_force_attempted;
	if (tx_quiet_required) {
		interrupt_status = ioread32(ctx->gpon + GPON_G_INT_STATUS);
		if (interrupt_status & INT_TX_ACTIVITY) {
			mask |= O3_RX_GUARD_DIAG_TX_STATUS;
			upstream_sources |= O3_RX_UPSTREAM_LATCH_TX_STATUS;
		}
		tx_burst_count =
			ioread32(ctx->gpon + GPON_DBG_TX_BST_CNT);
		if (tx_burst_count != result.mac_before.tx_burst_count) {
			mask |= O3_RX_GUARD_DIAG_TX_BURST;
			upstream_sources |= O3_RX_UPSTREAM_LATCH_TX_BURST;
		}
		ploamu_fifo_status =
			ioread32(ctx->gpon + GPON_G_PLOAMU_FIFO_STS);
		if (ploamu_fifo_status != result.mac_before.ploamu_fifo_status) {
			mask |= O3_RX_GUARD_DIAG_PLOAMU;
			upstream_sources |= O3_RX_UPSTREAM_LATCH_PLOAMU;
		}
		o3_rx_capture_tx_correlation(ctx, &tx_correlation);
		if (o3_rx_tx_correlation_upstream_sources(&tx_correlation)) {
			mask |= O3_RX_GUARD_DIAG_TX_GEM;
			upstream_sources |= O3_RX_UPSTREAM_LATCH_TX_GEM;
		}
		o3_rx_note_phy_raw_diagnostics(&tx_correlation);
		o3_rx_latch_upstream_activity(upstream_sources);
	}
	allow_rdm_dly = expected_state == ACTIVATION_O3 ||
			result.activation_o3_valid;
	identity_ok = o3_rx_identity_matches(
		ioread32(ctx->gpon + GPON_G_ONU_ID),
		ioread32(ctx->gpon + GPON_G_VENDOR_ID),
		ioread32(ctx->gpon + GPON_G_VS_SN));
	sn_msg_cfg_ok = o3_rx_sn_msg_cfg_matches(
		ioread32(ctx->gpon + GPON_G_SN_MSG_CFG), allow_rdm_dly);
	if (!identity_ok || !sn_msg_cfg_ok)
		mask |= O3_RX_GUARD_DIAG_IDENTITY;
	if (ioread32(ctx->gpon + GPON_G_GBL_CFG) !=
	    result.mac_before.global_config)
		mask |= O3_RX_GUARD_DIAG_GLOBAL;
	ploamu_control =
		ioread32(ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
	if (!o3_rx_ploamu_control_matches(ploamu_control))
		mask |= O3_RX_GUARD_DIAG_PLOAMU_CTRL;
	if (regmap_read(ctx->scu, EN751221_SCU_WAN_CONF, &value))
		mask |= O3_RX_GUARD_DIAG_SCU_READ;
	else if (value != result.wan_gpon)
		mask |= O3_RX_GUARD_DIAG_WAN;

	result.gpon_reads += tx_quiet_required ? 11 : 8;
	return mask;
}

static int o3_rx_static_guard(struct o3_rx_context *ctx,
			      unsigned int expected_state)
{
	return o3_rx_guard_common(ctx, expected_state, true);
}

static int o3_rx_o3_guard(struct o3_rx_context *ctx)
{
	return o3_rx_guard_common(ctx, ACTIVATION_O3, true);
}

static int o3_rx_restored_o1_guard(struct o3_rx_context *ctx)
{
	return o3_rx_guard_common(ctx, ACTIVATION_O1, true);
}

static int o3_rx_cleanup_restored_o1_guard(struct o3_rx_context *ctx)
{
	return o3_rx_cleanup_guard_common_internal(ctx, ACTIVATION_O1, true);
}

/*
 * GPON_O3_O4_PLOAMU_CTRL is deliberately outside the formatter allow-list.
 * This dedicated path is the sole exception: after an exact O1 baseline and
 * full RX-only guard, an explicit read-only module option may set bit 0 while
 * preserving every other bit.  No clear operation is permitted here.
 */
static int o3_rx_force_software_ploamu_control(struct o3_rx_context *ctx)
{
	u32 readback;
	int err;

	result.ploamu_control_force_attempted = true;
	iowrite32(result.ploamu_control_forced,
		  ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
	result.gpon_writes++;
	result.ploamu_control_writes++;
	readback = ioread32(ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
	result.gpon_reads++;
	result.ploamu_control_program_readback = readback;
	if (readback != result.ploamu_control_forced)
		return -EIO;

	result.ploamu_control_forced_valid = true;
	err = o3_rx_static_guard(ctx, ACTIVATION_O1);
	if (err)
		return err;

	return 0;
}

static bool o3_rx_write_allowed(const struct o3_rx_context *ctx,
				void __iomem *base, u32 offset)
{
	if (base == ctx->gpon)
		return offset == GPON_G_PLOU_GUARD_BIT ||
		       offset == GPON_G_PLOU_PREAMBLE12 ||
		       offset == GPON_G_PRE_ASSIGNED_DELAY;
	if (base == ctx->xpon)
		return offset == XPON_GPON_PREAMBLE ||
		       offset == XPON_GPON_DELIMITER_GUARD ||
		       offset == XPON_GPON_EXT_PREAMBLE;

	return false;
}

static int o3_rx_write_checked(struct o3_rx_context *ctx, void __iomem *base,
			       u32 offset, u32 value,
			       unsigned int expected_state, bool gpon_register)
{
	int err;

	if (!o3_rx_write_allowed(ctx, base, offset))
		return -EPERM;

	err = o3_rx_guard_common(ctx, expected_state, true);
	if (err)
		return err;
	iowrite32(value, base + offset);
	if (gpon_register)
		result.gpon_writes++;
	else
		result.xpon_formatter_writes++;
	if (ioread32(base + offset) != value)
		return -EIO;
	if (gpon_register)
		result.gpon_reads++;

	return o3_rx_guard_common(ctx, expected_state, true);
}

static int o3_rx_validate_initial_records(
				  struct o3_rx_upstream_overhead *overhead)
{
	result.record_class[0] = o3_rx_classify_record(result.ploamd_words[0]);
	result.record_class[1] = o3_rx_classify_record(result.ploamd_words[1]);
	result.record_count = O3_RX_INITIAL_RECORDS;
	o3_rx_count_record(0);
	o3_rx_count_record(1);
	result.initial_records_identical =
		!memcmp(result.ploamd_words[0], result.ploamd_words[1],
			sizeof(result.ploamd_words[0]));
	if (result.record_class[0] != O3_RX_RECORD_UPSTREAM_OVERHEAD ||
	    result.record_class[1] != O3_RX_RECORD_UPSTREAM_OVERHEAD ||
	    !result.initial_records_identical ||
	    (o3_rx_record_byte(result.ploamd_words[0], 9) & GENMASK(7, 6)))
		return -EBADMSG;

	o3_rx_parse_upstream_overhead(result.ploamd_words[0], overhead);
	return 0;
}

static int o3_rx_program_initial_formatter(
				 struct o3_rx_context *ctx,
				 const struct o3_rx_upstream_overhead *overhead)
{
	struct o3_rx_formatter_snapshot expected;
	u32 delimiter;
	int err;

	o3_rx_capture_formatter(ctx, &result.formatter_before);
	result.formatter_before_valid = true;
	expected = result.formatter_before;
	expected.phy_preamble = (u32)overhead->preamble3_pattern << 24 |
				(u32)overhead->preamble1 << 16 |
				(u32)overhead->preamble2 << 8 |
				O3_RX_PHY_GUARD_BITS;
	delimiter = (u32)overhead->delimiter[0] << 16 |
		    (u32)overhead->delimiter[1] << 8 |
		    overhead->delimiter[2];
	expected.phy_delimiter_guard =
		(u32)O3_RX_PHY_GUARD_PATTERN << 24 | delimiter;
	expected.mac_guard =
		(expected.mac_guard & ~O3_RX_MAC_GUARD_BITS) |
		O3_RX_PHY_GUARD_BITS;
	expected.mac_preamble12 =
		(expected.mac_preamble12 &
		 ~(O3_RX_MAC_PREAMBLE1 | O3_RX_MAC_PREAMBLE2)) |
		FIELD_PREP(O3_RX_MAC_PREAMBLE1, overhead->preamble1) |
		FIELD_PREP(O3_RX_MAC_PREAMBLE2, overhead->preamble2);
	expected.mac_pre_delay =
		(expected.mac_pre_delay &
		 ~(O3_RX_MAC_PRE_DELAY | O3_RX_MAC_PRE_DELAY_ENABLE)) |
		FIELD_PREP(O3_RX_MAC_PRE_DELAY, overhead->delay_time) |
		(overhead->delay_mode ? O3_RX_MAC_PRE_DELAY_ENABLE : 0);
	expected.phy_ext_preamble =
		(expected.phy_ext_preamble & ~O3_RX_PHY_OPER_RANGED) |
		FIELD_PREP(O3_RX_PHY_OPER_RANGED, O3_RX_PHY_OPER_O3_O4);

	result.formatter_program_attempted = true;
	err = o3_rx_write_checked(ctx, ctx->xpon, XPON_GPON_PREAMBLE,
				  expected.phy_preamble, ACTIVATION_O1, false);
	if (err)
		return err;
	err = o3_rx_write_checked(ctx, ctx->xpon,
				  XPON_GPON_DELIMITER_GUARD,
				  expected.phy_delimiter_guard,
				  ACTIVATION_O1, false);
	if (err)
		return err;
	err = o3_rx_write_checked(ctx, ctx->gpon, GPON_G_PLOU_GUARD_BIT,
				  expected.mac_guard, ACTIVATION_O1, true);
	if (err)
		return err;
	err = o3_rx_write_checked(ctx, ctx->gpon, GPON_G_PLOU_PREAMBLE12,
				  expected.mac_preamble12, ACTIVATION_O1, true);
	if (err)
		return err;
	err = o3_rx_write_checked(ctx, ctx->gpon,
				  GPON_G_PRE_ASSIGNED_DELAY,
				  expected.mac_pre_delay, ACTIVATION_O1, true);
	if (err)
		return err;
	err = o3_rx_write_checked(ctx, ctx->xpon, XPON_GPON_EXT_PREAMBLE,
				  expected.phy_ext_preamble,
				  ACTIVATION_O1, false);
	if (err)
		return err;

	o3_rx_capture_formatter(ctx, &result.formatter_programmed);
	if (!o3_rx_formatter_equal(&result.formatter_programmed, &expected))
		return -EIO;
	result.formatter_programmed_valid = true;

	return 0;
}

static int o3_rx_enter_o3(struct o3_rx_context *ctx)
{
	u32 value = (result.activation_before & ~ACTIVATION_STATE) |
		    ACTIVATION_O3;
	int err;

	err = o3_rx_static_guard(ctx, ACTIVATION_O1);
	if (err)
		return err;
	result.activation_o3_attempted = true;
	iowrite32(value, ctx->gpon + GPON_G_ACTIVATION_ST);
	result.gpon_writes++;
	result.activation_o3 = ioread32(ctx->gpon + GPON_G_ACTIVATION_ST);
	result.gpon_reads++;
	if (result.activation_o3 != value)
		return -EIO;
	/* This is a readback fact, not a statement that the post-write guard passed. */
	result.activation_o3_valid = true;
	err = o3_rx_o3_guard(ctx);
	result.activation_o3_guard_result = err;
	if (err)
		result.activation_o3_guard_mask =
			o3_rx_diagnose_guard(ctx, ACTIVATION_O3, false);
	if (err)
		return err;
	return 0;
}

static int o3_rx_capture_complete_record(struct o3_rx_context *ctx,
					 unsigned int index)
{
	unsigned long flags;
	u64 pop_start_ns = ktime_get_ns();
	unsigned int formatter_writes_before;
	unsigned int formatter_writes_after;
	u32 fifo_status_before;
	u32 fifo_status_after;
	u32 level_before;
	u32 level_after;
	int err;

	err = o3_rx_o3_guard(ctx);
	if (err)
		return err;

	preempt_disable();
	local_irq_save(flags);
	fifo_status_before = ioread32(ctx->gpon + GPON_G_PLOAMD_FIFO_STS);
	if (fifo_status_before & PLOAMD_FIFO_OVERRUN) {
		local_irq_restore(flags);
		preempt_enable();
		result.stop_reason = O3_RX_REASON_O3_FIFO_OVERRUN;
		return -EOVERFLOW;
	}
	level_before = FIELD_GET(PLOAM_FIFO_LEVEL, fifo_status_before);
	if (level_before < O3_RX_PLOAMD_WORDS_PER_RECORD) {
		local_irq_restore(flags);
		preempt_enable();
		return -EAGAIN;
	}
	if (level_before % O3_RX_PLOAMD_WORDS_PER_RECORD) {
		local_irq_restore(flags);
		preempt_enable();
		result.stop_reason = O3_RX_REASON_O3_FIFO_PARTIAL;
		return -EIO;
	}
	result.ploamd_words[index][0] =
		ioread32(ctx->gpon + GPON_G_PLOAMD_RDATA);
	result.ploamd_words[index][1] =
		ioread32(ctx->gpon + GPON_G_PLOAMD_RDATA);
	result.ploamd_words[index][2] =
		ioread32(ctx->gpon + GPON_G_PLOAMD_RDATA);
	fifo_status_after = ioread32(ctx->gpon + GPON_G_PLOAMD_FIFO_STS);
	level_after = FIELD_GET(PLOAM_FIFO_LEVEL, fifo_status_after);
	local_irq_restore(flags);
	preempt_enable();
	result.gpon_reads += 5;
	result.fifo_data_reads += O3_RX_PLOAMD_WORDS_PER_RECORD;
	if (ktime_get_ns() - pop_start_ns >=
	    (u64)O3_RX_FIFO_POP_HARD_US * NSEC_PER_USEC) {
		result.stop_reason = O3_RX_REASON_FIFO_POP_SLOW;
		return -ETIME;
	}
	if (fifo_status_after & PLOAMD_FIFO_OVERRUN) {
		result.stop_reason = O3_RX_REASON_O3_FIFO_OVERRUN;
		return -EOVERFLOW;
	}
	if (level_before - level_after != O3_RX_PLOAMD_WORDS_PER_RECORD ||
	    level_after % O3_RX_PLOAMD_WORDS_PER_RECORD) {
		result.stop_reason = O3_RX_REASON_O3_FIFO_PARTIAL;
		return -EIO;
	}

	result.record_class[index] =
		o3_rx_classify_record(result.ploamd_words[index]);
	result.record_count = index + 1;
	o3_rx_count_record(index);
	if (index == O3_RX_INITIAL_RECORDS)
		result.third_record_matches =
			!memcmp(result.ploamd_words[0],
				result.ploamd_words[index],
				sizeof(result.ploamd_words[index]));
	if (result.record_class[index] != O3_RX_RECORD_EXT_BURST_LENGTH)
		return o3_rx_o3_guard(ctx);

	/*
	 * Extended Burst Length is deliberately classification-only.  Bracket its
	 * complete handling with the central MAC/xPON write counters so the status
	 * oracle cannot silently remain zero if a formatter write is added here.
	 */
	formatter_writes_before =
		result.gpon_writes + result.xpon_formatter_writes;
	err = o3_rx_o3_guard(ctx);
	formatter_writes_after =
		result.gpon_writes + result.xpon_formatter_writes;
	result.extended_burst_formatter_writes +=
		formatter_writes_after - formatter_writes_before;
	if (result.extended_burst_formatter_writes) {
		result.stop_reason = O3_RX_REASON_O3_EXT_BURST_WRITE;
		return -EPERM;
	}

	return err;
}

/*
 * A one-count MAC TX-burst delta remains an immediate observation failure.
 * This oracle can only latch that already-observed value, either alone or
 * paired with an exact bit-31-only PLOAMu underrun transition, for
 * risk-reducing O3-to-O1/formatter cleanup.  It never accepts standalone
 * PLOAMu movement, permits observation to continue or permits O3/O4 PLOAMu
 * control to return to hardware-auto.
 */
static bool o3_rx_try_latch_cleanup_class(
	const struct gpon_mac_snapshot *probe,
	const struct xpon_guard_snapshot *guard,
	u32 upstream_sources)
{
	enum o3_rx_cleanup_class candidate = O3_RX_CLEANUP_NONE;
	u32 baseline_ploamu = result.mac_before.ploamu_fifo_status;
	u32 ploamu_delta = probe->ploamu_fifo_status ^ baseline_ploamu;
	u32 mask = 0;

	result.cleanup_tx_burst_latch_attempts++;
	if (upstream_sources == O3_RX_UPSTREAM_LATCH_TX_BURST &&
	    probe->ploamu_fifo_status == baseline_ploamu) {
		candidate = O3_RX_CLEANUP_TX_BURST_PLUS_ONE;
	} else if (upstream_sources ==
		   (O3_RX_UPSTREAM_LATCH_TX_BURST |
		    O3_RX_UPSTREAM_LATCH_PLOAMU) &&
		   !(baseline_ploamu & PLOAMU_FIFO_UNDERRUN) &&
		   ploamu_delta == PLOAMU_FIFO_UNDERRUN &&
		   probe->ploamu_fifo_status & PLOAMU_FIFO_UNDERRUN) {
		candidate =
			O3_RX_CLEANUP_TX_BURST_PLUS_ONE_PLOAMU_UNDERRUN;
	} else {
		mask |= O3_RX_CLEANUP_LATCH_SOURCE_CLASS;
	}
	if (probe->tx_burst_count !=
	    result.mac_before.tx_burst_count + 1)
		mask |= O3_RX_CLEANUP_LATCH_DELTA;
	if (verify_xpon_guard(guard))
		mask |= O3_RX_CLEANUP_LATCH_XPON;
	if (FIELD_GET(ACTIVATION_STATE, probe->activation_status) !=
	    ACTIVATION_O3)
		mask |= O3_RX_CLEANUP_LATCH_STATE;
	if (probe->interrupt_enable)
		mask |= O3_RX_CLEANUP_LATCH_IRQ_ENABLE;
	if (probe->interrupt_status & INT_TX_ACTIVITY)
		mask |= O3_RX_CLEANUP_LATCH_TX_STATUS;
	if (candidate == O3_RX_CLEANUP_TX_BURST_PLUS_ONE &&
	    probe->ploamu_fifo_status != baseline_ploamu)
		mask |= O3_RX_CLEANUP_LATCH_PLOAMU;
	if (candidate ==
	    O3_RX_CLEANUP_TX_BURST_PLUS_ONE_PLOAMU_UNDERRUN &&
	    ((baseline_ploamu & PLOAMU_FIFO_UNDERRUN) ||
	     ploamu_delta != PLOAMU_FIFO_UNDERRUN ||
	     !(probe->ploamu_fifo_status & PLOAMU_FIFO_UNDERRUN)))
		mask |= O3_RX_CLEANUP_LATCH_PLOAMU_DELTA;
	if (!result.ploamu_control_force_attempted ||
	    !result.ploamu_control_forced_valid ||
	    probe->o3_o4_ploamu_control != result.ploamu_control_forced)
		mask |= O3_RX_CLEANUP_LATCH_PLOAMU_CTRL;
	if (!o3_rx_snapshot_matches(probe, true))
		mask |= O3_RX_CLEANUP_LATCH_IDENTITY;
	if (probe->global_config != result.mac_before.global_config)
		mask |= O3_RX_CLEANUP_LATCH_GLOBAL;
	if (probe->interrupt_status & ~O3_RX_O3_ALLOWED_STATUS)
		mask |= O3_RX_CLEANUP_LATCH_STATUS;
	if (probe->ploamd_fifo_status & PLOAMD_FIFO_OVERRUN)
		mask |= O3_RX_CLEANUP_LATCH_DS_OVERRUN;
	if (FIELD_GET(PLOAM_FIFO_LEVEL, probe->ploamd_fifo_status) %
	    O3_RX_PLOAMD_WORDS_PER_RECORD)
		mask |= O3_RX_CLEANUP_LATCH_DS_PARTIAL;
	if (!probe->tx_gem_valid ||
	    !result.mac_before.tx_gem_valid ||
	    probe->tx_correlation.mac_tx_gem_count !=
	    result.mac_before.tx_correlation.mac_tx_gem_count)
		mask |= O3_RX_CLEANUP_LATCH_TX_GEM;
	result.cleanup_tx_burst_latch_reject_mask |= mask;
	if (mask)
		return false;

	result.cleanup_tx_burst_latched_value = probe->tx_burst_count;
	result.cleanup_tx_burst_guard_last = probe->tx_burst_count;
	result.cleanup_ploamu_accepted_status = probe->ploamu_fifo_status;
	result.cleanup_ploamu_accepted_delta = ploamu_delta;
	result.cleanup_ploamu_guard_last = probe->ploamu_fifo_status;
	result.cleanup_ploamd_accepted_status = probe->ploamd_fifo_status;
	result.cleanup_ploamd_guard_last = probe->ploamd_fifo_status;
	result.cleanup_class = candidate;
	result.cleanup_tx_burst_latched = true;
	/*
	 * The latch is permanent for this module lifetime.  Even exact cleanup
	 * cannot turn an internal MAC burst indication into a passing result.
	 */
	result.cold_power_cycle_required = true;
	result.restore_unsafe = true;
	return true;
}

static u32 o3_rx_upstream_activity_sources(
	const struct gpon_mac_snapshot *probe)
{
	u32 sources = 0;

	if (!probe->tx_gem_valid || !result.mac_before.tx_gem_valid) {
		sources |= O3_RX_UPSTREAM_LATCH_TX_GEM;
	} else {
		sources = o3_rx_tx_correlation_upstream_sources(
			&probe->tx_correlation);
	}
	if (probe->tx_correlation_valid)
		o3_rx_note_phy_raw_diagnostics(&probe->tx_correlation);
	if (probe->interrupt_status & INT_TX_ACTIVITY)
		sources |= O3_RX_UPSTREAM_LATCH_TX_STATUS;
	if (probe->tx_burst_count != result.mac_before.tx_burst_count)
		sources |= O3_RX_UPSTREAM_LATCH_TX_BURST;
	if (probe->ploamu_fifo_status != result.mac_before.ploamu_fifo_status)
		sources |= O3_RX_UPSTREAM_LATCH_PLOAMU;

	return sources;
}

static int o3_rx_observe_o3(struct o3_rx_context *ctx)
{
	struct gpon_mac_snapshot probe = {};
	struct xpon_guard_snapshot guard = {};
	u64 observe_start_ns = ktime_get_ns();
	u64 deadline_ns = observe_start_ns +
			  (u64)result.observe_ms_used * NSEC_PER_MSEC;
	u32 fifo_level;
	u32 fifo_status;
	u32 upstream_sources;
	u64 now_ns;
	int err;

	result.o3_observation_started = true;
	for (;;) {
		if (kthread_should_stop())
			return -EINTR;
		now_ns = ktime_get_ns();
		if (result.o3_poll_count >= O3_RX_MAX_POLL_COUNT) {
			result.stop_reason = O3_RX_REASON_O3_POLL_LIMIT;
			return -ENOSPC;
		}
		if (result.o3_last_poll_ns) {
			u64 gap_ns = now_ns - result.o3_last_poll_ns;

			if (gap_ns > result.o3_max_poll_gap_ns)
				result.o3_max_poll_gap_ns = gap_ns;
			if (gap_ns >
			    (u64)GPON_POLL_HARD_GAP_US * NSEC_PER_USEC) {
				result.stop_reason = O3_RX_REASON_O3_POLL_GAP;
				return -ETIME;
			}
		}
		result.o3_last_poll_ns = now_ns;
		result.o3_poll_count++;
		capture_gpon_probe(ctx, &probe, observe_start_ns);
		capture_xpon_guard(ctx, &guard);
		/*
		 * Preserve only coarse event evidence, then abort immediately on
		 * every indication that an upstream path may have moved.
		 */
		result.o3_interrupt_seen |= probe.interrupt_status;
		result.sn_request_seen |= !!(probe.interrupt_status &
						  INT_SN_REQ_RECV);
		result.sn_internal_send_seen |= !!(probe.interrupt_status &
							INT_SN_ONU_SEND_O3);
		result.o3_tx_burst_changed =
			probe.tx_burst_count != result.mac_before.tx_burst_count;
		result.o3_tx_burst_last = probe.tx_burst_count;
		result.o3_tx_correlation_last = probe.tx_correlation;
		result.o3_tx_correlation_valid =
			probe.tx_correlation_valid;
		result.o3_ploamu_status_changed =
			probe.ploamu_fifo_status !=
			result.mac_before.ploamu_fifo_status;
		result.o3_ploamu_status_last = probe.ploamu_fifo_status;
		upstream_sources = o3_rx_upstream_activity_sources(&probe);
		o3_rx_latch_upstream_activity(upstream_sources);
		err = verify_xpon_guard(&guard);
		if (err) {
			result.stop_reason = O3_RX_REASON_O3_GUARD;
			return err;
		}
		if (upstream_sources) {
			if (upstream_sources ==
			    O3_RX_UPSTREAM_LATCH_TX_BURST ||
			    upstream_sources ==
			    (O3_RX_UPSTREAM_LATCH_TX_BURST |
			     O3_RX_UPSTREAM_LATCH_PLOAMU))
				o3_rx_try_latch_cleanup_class(&probe, &guard,
							 upstream_sources);
			o3_rx_set_upstream_stop_reason(upstream_sources);
			return -EPERM;
		}
		if (!o3_rx_ploamu_control_matches(
			    probe.o3_o4_ploamu_control)) {
			result.stop_reason =
				O3_RX_REASON_PLOAMU_CTRL_INVARIANT;
			return -EUCLEAN;
		}
		if (FIELD_GET(ACTIVATION_STATE, probe.activation_status) !=
		    ACTIVATION_O3 || probe.interrupt_enable ||
		    !o3_rx_snapshot_matches(&probe, true) ||
		    probe.global_config != result.mac_before.global_config ||
		    probe.interrupt_status & ~O3_RX_O3_ALLOWED_STATUS) {
			result.stop_reason = O3_RX_REASON_O3_GUARD;
			return -EPERM;
		}
		if (probe.ploamd_fifo_status & PLOAMD_FIFO_OVERRUN) {
			result.stop_reason = O3_RX_REASON_O3_FIFO_OVERRUN;
			return -EOVERFLOW;
		}

		fifo_status = probe.ploamd_fifo_status;
		fifo_level = FIELD_GET(PLOAM_FIFO_LEVEL, fifo_status);
		while (fifo_level >= O3_RX_PLOAMD_WORDS_PER_RECORD) {
			if (result.record_count >= result.max_records_used) {
				result.stop_reason = O3_RX_REASON_O3_RECORD_LIMIT;
				return -ENOSPC;
			}
			err = o3_rx_capture_complete_record(ctx,
						    result.record_count);
			if (err == -EAGAIN)
				break;
			if (err) {
				if (result.stop_reason == O3_RX_REASON_NONE)
					result.stop_reason = O3_RX_REASON_O3_GUARD;
				return err;
			}
			fifo_status =
				ioread32(ctx->gpon + GPON_G_PLOAMD_FIFO_STS);
			result.gpon_reads++;
			if (fifo_status & PLOAMD_FIFO_OVERRUN) {
				result.stop_reason = O3_RX_REASON_O3_FIFO_OVERRUN;
				return -EOVERFLOW;
			}
			fifo_level = FIELD_GET(PLOAM_FIFO_LEVEL, fifo_status);
		}
		if (fifo_level % O3_RX_PLOAMD_WORDS_PER_RECORD) {
			result.stop_reason = O3_RX_REASON_O3_FIFO_PARTIAL;
			return -EIO;
		}
		if (ktime_get_ns() >= deadline_ns ||
		    (result.extended_burst_seen && result.sn_request_seen))
			break;
		usleep_range(O3_RX_POLL_MIN_US, O3_RX_POLL_MAX_US);
	}

	capture_gpon_checkpoint(ctx, &result.o3_final, observe_start_ns);
	result.o3_final_valid = true;
	result.o3_interrupt_seen |= result.o3_final.interrupt_status;
	result.sn_request_seen |= !!(result.o3_final.interrupt_status &
				      INT_SN_REQ_RECV);
	result.sn_internal_send_seen |=
		!!(result.o3_final.interrupt_status & INT_SN_ONU_SEND_O3);
	result.o3_tx_burst_changed =
		result.o3_final.tx_burst_count !=
		result.mac_before.tx_burst_count;
	result.o3_tx_burst_last = result.o3_final.tx_burst_count;
	result.o3_tx_correlation_last =
		result.o3_final.tx_correlation;
	result.o3_tx_correlation_valid =
		result.o3_final.tx_correlation_valid;
	result.o3_ploamu_status_changed =
		result.o3_final.ploamu_fifo_status !=
		result.mac_before.ploamu_fifo_status;
	result.o3_ploamu_status_last =
		result.o3_final.ploamu_fifo_status;
	upstream_sources = o3_rx_upstream_activity_sources(&result.o3_final);
	o3_rx_latch_upstream_activity(upstream_sources);
	if (upstream_sources) {
		if (upstream_sources == O3_RX_UPSTREAM_LATCH_TX_BURST ||
		    upstream_sources ==
		    (O3_RX_UPSTREAM_LATCH_TX_BURST |
		     O3_RX_UPSTREAM_LATCH_PLOAMU)) {
			capture_xpon_guard(ctx, &guard);
			o3_rx_try_latch_cleanup_class(&result.o3_final, &guard,
						     upstream_sources);
		}
		o3_rx_set_upstream_stop_reason(upstream_sources);
		return -EPERM;
	}
	if (!o3_rx_ploamu_control_matches(
		    result.o3_final.o3_o4_ploamu_control)) {
		result.stop_reason = O3_RX_REASON_PLOAMU_CTRL_INVARIANT;
		return -EUCLEAN;
	}
	if (FIELD_GET(ACTIVATION_STATE,
		      result.o3_final.activation_status) != ACTIVATION_O3 ||
	    result.o3_final.interrupt_enable ||
	    result.o3_final.global_config != result.mac_before.global_config ||
	    result.o3_final.interrupt_status & ~O3_RX_O3_ALLOWED_STATUS) {
		result.stop_reason = O3_RX_REASON_O3_GUARD;
		return -EPERM;
	}
	if (!o3_rx_snapshot_matches(&result.o3_final, true)) {
		result.stop_reason = O3_RX_REASON_O3_GUARD;
		return -EUCLEAN;
	}
	if (result.o3_final.ploamd_fifo_status & PLOAMD_FIFO_OVERRUN) {
		result.stop_reason = O3_RX_REASON_O3_FIFO_OVERRUN;
		return -EOVERFLOW;
	}
	if (FIELD_GET(PLOAM_FIFO_LEVEL,
		      result.o3_final.ploamd_fifo_status) %
	    O3_RX_PLOAMD_WORDS_PER_RECORD) {
		result.stop_reason = O3_RX_REASON_O3_FIFO_PARTIAL;
		return -EIO;
	}
	capture_xpon_guard(ctx, &result.o3_guard_final);
	result.o3_guard_final_valid = true;
	err = verify_xpon_guard(&result.o3_guard_final);
	if (err) {
		result.stop_reason = O3_RX_REASON_O3_GUARD;
		return err;
	}
	result.stop_reason = O3_RX_REASON_O3_OBSERVATION_COMPLETE;
	return 0;
}

static void o3_rx_keep_first_error(int *first_error, int err)
{
	if (err && !*first_error)
		*first_error = err;
}

/*
 * Restore the complete saved word only while the GPON aperture is selected
 * and a baseline-clean O1/RX-only proof succeeds.  A latched MAC TX-burst
 * delta is never accepted here: retain software control, pin the module and
 * require physical power removal.
 */
static int o3_rx_restore_ploamu_control(struct o3_rx_context *ctx)
{
	int first_error = 0;
	int err;

	if (!result.mac_before_valid)
		return 0;

	if (!result.ploamu_control_force_attempted) {
		result.ploamu_control_after =
			ioread32(ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
		result.gpon_reads++;
		if (result.ploamu_control_after !=
		    result.ploamu_control_before) {
			result.cold_power_cycle_required = true;
			return -EUCLEAN;
		}
		return 0;
	}

	result.ploamu_control_restore_attempted = true;
	if (result.upstream_activity_latched) {
		result.ploamu_control_restore_skipped_guard = true;
		result.ploamu_control_restore_skipped_upstream_latch = true;
		result.ploamu_control_restore_pre_guard_result = -EPERM;
		result.restore_guard_checks++;
		result.restore_guard_failures++;
		result.cold_power_cycle_required = true;
		result.restore_unsafe = true;
		result.ploamu_control_after =
			ioread32(ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
		result.gpon_reads++;
		if (result.ploamu_control_after !=
		    result.ploamu_control_forced)
			return -EUCLEAN;
		return -EPERM;
	}
	if (result.formatter_program_attempted &&
	    !result.formatter_restored_valid) {
		result.ploamu_control_restore_skipped_guard = true;
		result.ploamu_control_restore_skipped_formatter = true;
		result.ploamu_control_restore_pre_guard_result = -EUCLEAN;
		result.restore_guard_checks++;
		result.restore_guard_failures++;
		result.cold_power_cycle_required = true;
		result.restore_unsafe = true;
		result.ploamu_control_after =
			ioread32(ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
		result.gpon_reads++;
		return -EUCLEAN;
	}

	result.restore_guard_checks++;
	err = o3_rx_guard_common_internal(ctx, ACTIVATION_O1, true, true);
	result.ploamu_control_restore_pre_guard_result = err;
	if (err) {
		u32 activation;

		result.restore_guard_failures++;
		result.ploamu_control_restore_skipped_guard = true;
		result.cold_power_cycle_required = true;
		result.restore_unsafe = true;
		activation =
			ioread32(ctx->gpon + GPON_G_ACTIVATION_ST);
		result.gpon_reads++;
		result.ploamu_control_restore_skipped_no_o1 =
			FIELD_GET(ACTIVATION_STATE, activation) != ACTIVATION_O1;
		result.ploamu_control_after =
			ioread32(ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
		result.gpon_reads++;
		return err;
	}

	iowrite32(result.ploamu_control_before,
		  ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
	result.restore_write_attempts++;
	result.gpon_writes++;
	result.ploamu_control_writes++;
	result.ploamu_control_after =
		ioread32(ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
	result.gpon_reads++;
	if (result.ploamu_control_after != result.ploamu_control_before) {
		result.restore_readback_failures++;
		o3_rx_keep_first_error(&first_error, -EIO);
	} else {
		result.ploamu_control_forced_valid = false;
		result.ploamu_control_restored_valid = true;
	}

	result.restore_guard_checks++;
	err = o3_rx_guard_common_internal(ctx, ACTIVATION_O1, true,
					  result.ploamu_control_restored_valid);
	result.ploamu_control_restore_post_guard_result = err;
	if (err) {
		result.restore_guard_failures++;
		o3_rx_keep_first_error(&first_error, err);
	}

	if (first_error) {
		result.cold_power_cycle_required = true;
		result.restore_unsafe = true;
	}

	return first_error;
}

/*
 * Only an exact stable +1 TX-burst latch, alone or paired with an exact stable
 * bit-31-only PLOAMu underrun latch, may enter this risk-reducing cleanup
 * path.  Every pre-write guard is authoritative: a GPIO16, xPON, mux, state,
 * identity, control, IRQ, FIFO or counter failure blocks the write and leaves
 * the module pinned for a physical power cut.
 */
static int o3_rx_restore_write(struct o3_rx_context *ctx, void __iomem *base,
			       u32 offset, u32 value, bool gpon_register,
			       unsigned int state_before,
			       unsigned int state_after)
{
	struct o3_rx_tx_correlation_snapshot tx_correlation;
	bool activation = base == ctx->gpon &&
			  offset == GPON_G_ACTIVATION_ST;
	u32 upstream_sources;
	int first_error = 0;
	int err;

	if (!activation && !o3_rx_write_allowed(ctx, base, offset))
		return -EPERM;
	if (result.upstream_activity_latched &&
	    !o3_rx_cleanup_tx_burst_eligible()) {
		o3_rx_capture_tx_correlation(ctx, &tx_correlation);
		o3_rx_record_cleanup_tx_correlation(&tx_correlation);
		upstream_sources =
			o3_rx_tx_correlation_upstream_sources(&tx_correlation);
		o3_rx_latch_upstream_activity(upstream_sources);
		if (upstream_sources)
			o3_rx_set_upstream_stop_reason(upstream_sources);
		return -EPERM;
	}

	result.restore_guard_checks++;
	err = o3_rx_cleanup_guard_common_internal(ctx, state_before, true);
	if (err) {
		result.restore_guard_failures++;
		return err;
	}

	iowrite32(value, base + offset);
	result.restore_write_attempts++;
	if (gpon_register)
		result.gpon_writes++;
	else
		result.xpon_formatter_writes++;
	if (ioread32(base + offset) != value) {
		result.restore_readback_failures++;
		o3_rx_keep_first_error(&first_error, -EIO);
	}
	if (gpon_register)
		result.gpon_reads++;

	result.restore_guard_checks++;
	err = o3_rx_cleanup_guard_common_internal(ctx, state_after, true);
	if (err) {
		result.restore_guard_failures++;
		o3_rx_keep_first_error(&first_error, err);
	}

	return first_error;
}

static int o3_rx_restore_formatter(struct o3_rx_context *ctx)
{
	const struct o3_rx_formatter_snapshot *before = &result.formatter_before;
	u32 activation;
	int first_error = 0;
	int err;

	if (!result.formatter_before_valid ||
	    !result.formatter_program_attempted)
		return 0;

	if (result.activation_o3_attempted) {
		err = o3_rx_restore_write(ctx, ctx->gpon,
					  GPON_G_ACTIVATION_ST,
					  result.activation_before, true,
					  ACTIVATION_O3, ACTIVATION_O1);
		o3_rx_keep_first_error(&first_error, err);
	}
	err = o3_rx_restore_write(ctx, ctx->xpon, XPON_GPON_EXT_PREAMBLE,
				  before->phy_ext_preamble, false,
				  ACTIVATION_O1, ACTIVATION_O1);
	o3_rx_keep_first_error(&first_error, err);
	err = o3_rx_restore_write(ctx, ctx->gpon, GPON_G_PRE_ASSIGNED_DELAY,
				  before->mac_pre_delay, true,
				  ACTIVATION_O1, ACTIVATION_O1);
	o3_rx_keep_first_error(&first_error, err);
	err = o3_rx_restore_write(ctx, ctx->gpon, GPON_G_PLOU_PREAMBLE12,
				  before->mac_preamble12, true,
				  ACTIVATION_O1, ACTIVATION_O1);
	o3_rx_keep_first_error(&first_error, err);
	err = o3_rx_restore_write(ctx, ctx->gpon, GPON_G_PLOU_GUARD_BIT,
				  before->mac_guard, true,
				  ACTIVATION_O1, ACTIVATION_O1);
	o3_rx_keep_first_error(&first_error, err);
	err = o3_rx_restore_write(ctx, ctx->xpon,
				  XPON_GPON_DELIMITER_GUARD,
				  before->phy_delimiter_guard, false,
				  ACTIVATION_O1, ACTIVATION_O1);
	o3_rx_keep_first_error(&first_error, err);
	err = o3_rx_restore_write(ctx, ctx->xpon, XPON_GPON_PREAMBLE,
				  before->phy_preamble, false,
				  ACTIVATION_O1, ACTIVATION_O1);
	o3_rx_keep_first_error(&first_error, err);
	activation = ioread32(ctx->gpon + GPON_G_ACTIVATION_ST);
	result.gpon_reads++;
	o3_rx_capture_formatter(ctx, &result.formatter_after);
	if (activation != result.activation_before ||
	    !o3_rx_formatter_equal(&result.formatter_after, before))
		o3_rx_keep_first_error(&first_error, -EIO);

	err = o3_rx_cleanup_restored_o1_guard(ctx);
	o3_rx_keep_first_error(&first_error, err);
	if (!first_error)
		result.formatter_restored_valid = true;

	return first_error;
}

static void capture_gpon_mac_base(struct o3_rx_context *ctx,
				  struct gpon_mac_snapshot *s,
				  u64 start_ns)
{
	s->elapsed_ns = ktime_get_ns() - start_ns;
	s->onu_id = ioread32(ctx->gpon + GPON_G_ONU_ID);
	s->global_config = ioread32(ctx->gpon + GPON_G_GBL_CFG);
	s->vendor_id = ioread32(ctx->gpon + GPON_G_VENDOR_ID);
	s->vs_sn = ioread32(ctx->gpon + GPON_G_VS_SN);
	s->sn_msg_cfg = ioread32(ctx->gpon + GPON_G_SN_MSG_CFG);
	s->interrupt_status = ioread32(ctx->gpon + GPON_G_INT_STATUS);
	s->interrupt_enable = ioread32(ctx->gpon + GPON_G_INT_ENABLE);
	s->ploamu_fifo_status =
		ioread32(ctx->gpon + GPON_G_PLOAMU_FIFO_STS);
	s->ploamd_fifo_status =
		ioread32(ctx->gpon + GPON_G_PLOAMD_FIFO_STS);
	s->activation_status =
		ioread32(ctx->gpon + GPON_G_ACTIVATION_ST);
	s->tx_burst_count = ioread32(ctx->gpon + GPON_DBG_TX_BST_CNT);
	o3_rx_capture_tx_correlation(ctx, &s->tx_correlation);
	s->tx_gem_valid = true;
	s->tx_correlation_valid = true;
	s->o3_o4_ploamu_control =
		ioread32(ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
	result.gpon_reads += 12;
}

static void capture_gpon_probe(struct o3_rx_context *ctx,
			       struct gpon_mac_snapshot *s,
			       u64 start_ns)
{
	s->elapsed_ns = ktime_get_ns() - start_ns;
	s->interrupt_status = ioread32(ctx->gpon + GPON_G_INT_STATUS);
	s->rx_gtc_count = ioread32(ctx->gpon + GPON_DBG_RX_GTC_CNT);
	s->interrupt_enable = ioread32(ctx->gpon + GPON_G_INT_ENABLE);
	s->activation_status =
		ioread32(ctx->gpon + GPON_G_ACTIVATION_ST);
	s->tx_burst_count = ioread32(ctx->gpon + GPON_DBG_TX_BST_CNT);
	o3_rx_capture_tx_correlation(ctx, &s->tx_correlation);
	s->tx_gem_valid = true;
	s->tx_correlation_valid = true;
	s->onu_id = ioread32(ctx->gpon + GPON_G_ONU_ID);
	s->global_config = ioread32(ctx->gpon + GPON_G_GBL_CFG);
	s->vendor_id = ioread32(ctx->gpon + GPON_G_VENDOR_ID);
	s->vs_sn = ioread32(ctx->gpon + GPON_G_VS_SN);
	s->sn_msg_cfg = ioread32(ctx->gpon + GPON_G_SN_MSG_CFG);
	s->ploamu_fifo_status =
		ioread32(ctx->gpon + GPON_G_PLOAMU_FIFO_STS);
	s->ploamd_fifo_status =
		ioread32(ctx->gpon + GPON_G_PLOAMD_FIFO_STS);
	s->o3_o4_ploamu_control =
		ioread32(ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
	result.gpon_reads += 13;
}

static void capture_gpon_checkpoint(struct o3_rx_context *ctx,
				    struct gpon_mac_snapshot *s,
				    u64 start_ns)
{
	capture_gpon_mac_base(ctx, s, start_ns);
	s->rx_gem_count = ioread32(ctx->gpon + GPON_DBG_RX_GEM_CNT);
	s->rx_crc_error_count =
		ioread32(ctx->gpon + GPON_DBG_RX_CRC_ERR_CNT);
	s->rx_gtc_count = ioread32(ctx->gpon + GPON_DBG_RX_GTC_CNT);
	s->rx_hec_one_error_count =
		ioread32(ctx->gpon + GPON_DBG_RX_HEC_ONE_ERR_CNT);
	s->rx_hec_two_error_count =
		ioread32(ctx->gpon + GPON_DBG_RX_HEC_TWO_ERR_CNT);
	s->rx_hec_uncorrectable_count =
		ioread32(ctx->gpon + GPON_DBG_RX_HEC_UNCORR_CNT);
	s->ds_spf_count = ioread32(ctx->gpon + GPON_DBG_DS_SPF_CNT);
	s->response_time = ioread32(ctx->gpon + GPON_G_RSP_TIME);
	s->mbi_stop = ioread32(ctx->gpon + GPON_G_MBI_STOP);
	s->dbg_cap_setting =
		ioread32(ctx->gpon + GPON_DBG_CAP_SETTING);
	s->dbg_delay = ioread32(ctx->gpon + GPON_DBG_DLY);
	s->dbg_idle_gem_threshold =
		ioread32(ctx->gpon + GPON_DBG_IDLE_GEM_THLD);
	s->dbg_ploamd_filter_in_o5 =
		ioread32(ctx->gpon + GPON_DBG_PLOAMD_FILTER_IN_O5);
	result.gpon_reads += 13;
}

static int check_observation(const struct gpon_mac_snapshot *mac,
			     const struct xpon_guard_snapshot *guard)
{
	u64 gap_ns = 0;
	u32 upstream_sources;
	int err;

	upstream_sources = o3_rx_upstream_activity_sources(mac);
	o3_rx_latch_upstream_activity(upstream_sources);
	if (result.checks)
		gap_ns = mac->elapsed_ns - result.last_check_elapsed_ns;
	else
		gap_ns = mac->elapsed_ns;
	if (gap_ns > result.max_check_gap_ns)
		result.max_check_gap_ns = gap_ns;
	result.last_check_elapsed_ns = mac->elapsed_ns;
	result.checks++;
	if (gap_ns > (u64)GPON_POLL_HARD_GAP_US * NSEC_PER_USEC) {
		result.stop_reason = O3_RX_REASON_POLL_GAP;
		return -ETIME;
	}

	if (FIELD_GET(ACTIVATION_STATE, mac->activation_status) !=
	    ACTIVATION_O2) {
		result.stop_reason = O3_RX_REASON_ACTIVATION_LEFT_O2;
		return -EPROTO;
	}
	if (mac->interrupt_enable) {
		result.stop_reason = O3_RX_REASON_INTERRUPT_ENABLED;
		return -EPERM;
	}
	if (upstream_sources) {
		o3_rx_set_upstream_stop_reason(upstream_sources);
		return -EPERM;
	}
	if (!o3_rx_snapshot_matches(mac, false)) {
		result.stop_reason = O3_RX_REASON_ONU_ID_CHANGED;
		return -EUCLEAN;
	}
	if (mac->global_config != result.mac_before.global_config) {
		result.stop_reason = O3_RX_REASON_GLOBAL_CONFIG_CHANGED;
		return -EUCLEAN;
	}
	if (!o3_rx_ploamu_control_matches(mac->o3_o4_ploamu_control)) {
		result.stop_reason = O3_RX_REASON_PLOAMU_CTRL_INVARIANT;
		return -EUCLEAN;
	}
	if (mac->ploamu_fifo_status !=
	    result.mac_before.ploamu_fifo_status) {
		result.stop_reason = O3_RX_REASON_PLOAMU_CHANGED;
		return -EPERM;
	}
	if (mac->ploamd_fifo_status & PLOAMD_FIFO_OVERRUN) {
		result.stop_reason = O3_RX_REASON_O3_FIFO_OVERRUN;
		return -EOVERFLOW;
	}

	err = verify_xpon_guard(guard);
	if (err) {
		result.stop_reason = O3_RX_REASON_XPON_GUARD;
		return err;
	}

	return 0;
}

static bool record_downstream_if_present(const struct gpon_mac_snapshot *mac)
{
	u32 trigger = 0;

	if (FIELD_GET(PLOAM_FIFO_LEVEL, mac->ploamd_fifo_status))
		trigger |= O3_RX_DS_FIFO_LEVEL;
	if (mac->interrupt_status & INT_PLOAMD_RECV)
		trigger |= O3_RX_DS_PLOAMD_STATUS;
	if (mac->interrupt_status & INT_SN_REQ_RECV)
		trigger |= O3_RX_DS_SN_REQ;
	if (mac->interrupt_status & INT_RANGING_REQ_RECV)
		trigger |= O3_RX_DS_RANGING_REQ;
	if (mac->interrupt_status & INT_SN_REQ_CRS)
		trigger |= O3_RX_DS_SN_REQ_CRS;
	if (!trigger)
		return false;

	if (!result.downstream_trigger_valid) {
		result.downstream_trigger = *mac;
		result.downstream_trigger_source = trigger;
		result.downstream_trigger_valid = true;
	}
	return true;
}

static int check_interrupt_status(const struct gpon_mac_snapshot *mac,
				  enum o3_rx_stop_reason reason)
{
	bool downstream = record_downstream_if_present(mac);
	u32 allowed = O3_RX_SAFE_STARTUP_MASK;
	u32 unsafe;

	if (downstream)
		allowed |= O3_RX_SAFE_DOWNSTREAM_IRQ_MASK;
	unsafe = mac->interrupt_status & ~allowed;
	if (unsafe) {
		if (!result.unsafe_status_valid) {
			result.unsafe_status = *mac;
			result.unsafe_interrupt_mask = unsafe;
			result.unsafe_status_valid = true;
		}
		result.stop_reason = reason;
		return -EIO;
	}
	if (downstream) {
		result.passive_trigger_polls++;
		result.stop_reason = O3_RX_REASON_DOWNSTREAM_PROGRESS;
		return 1;
	}

	return 0;
}

static int o3_rx_arm_single_pop(void)
{
	const struct gpon_mac_snapshot *mac = &result.downstream_trigger;
	u32 upstream_sources = o3_rx_upstream_activity_sources(mac);

	o3_rx_latch_upstream_activity(upstream_sources);
	if (upstream_sources) {
		result.trigger_gate_result = -EPERM;
		o3_rx_set_upstream_stop_reason(upstream_sources);
		return result.trigger_gate_result;
	}
	if (!result.downstream_trigger_valid || !result.first_gtc_valid ||
	    !result.startup_expected_match ||
	    result.downstream_trigger_source !=
	    O3_RX_EXACT_TRIGGER_SOURCE ||
	    mac->ploamd_fifo_status != O3_RX_FIFO_BEFORE_POP ||
	    mac->interrupt_status != O3_RX_PASSIVE_TRIGGER_STATUS ||
	    mac->interrupt_enable ||
	    mac->interrupt_status & INT_TX_ACTIVITY ||
	    mac->tx_burst_count != result.mac_before.tx_burst_count ||
	    !o3_rx_snapshot_matches(mac, false) ||
	    mac->global_config != result.mac_before.global_config ||
	    !o3_rx_ploamu_control_matches(mac->o3_o4_ploamu_control) ||
	    mac->ploamu_fifo_status !=
	    result.mac_before.ploamu_fifo_status ||
	    FIELD_GET(ACTIVATION_STATE, mac->activation_status) !=
	    ACTIVATION_O2) {
		result.trigger_gate_result = -EUCLEAN;
		result.stop_reason = O3_RX_REASON_TRIGGER_NOT_EXACT;
		return result.trigger_gate_result;
	}

	result.accepted_trigger_status = mac->interrupt_status;
	result.fifo_pop_armed = true;
	return 0;
}

static int verify_mac_after(const struct gpon_mac_snapshot *mac)
{
	u32 upstream_sources = o3_rx_upstream_activity_sources(mac);

	o3_rx_latch_upstream_activity(upstream_sources);
	if (upstream_sources) {
		o3_rx_set_upstream_stop_reason(upstream_sources);
		return -EPERM;
	}
	if (mac->activation_status != result.activation_before)
		return -EIO;
	if (mac->interrupt_enable ||
	    (mac->interrupt_status & INT_TX_ACTIVITY) ||
	    mac->tx_burst_count != result.mac_before.tx_burst_count)
		return -EPERM;
	if (!o3_rx_snapshot_matches(mac, false) ||
	    mac->global_config != result.mac_before.global_config ||
	    !o3_rx_ploamu_control_matches(mac->o3_o4_ploamu_control) ||
	    mac->ploamu_fifo_status != result.mac_before.ploamu_fifo_status)
		return -EUCLEAN;

	return 0;
}

static int verify_o1_pop_boundary(const struct gpon_mac_snapshot *mac)
{
	int err;

	err = verify_mac_after(mac);
	if (err)
		return err;
	if (mac->ploamd_fifo_status != O3_RX_FIFO_BEFORE_POP)
		return -ENODATA;
	if (mac->ploamd_fifo_status & PLOAMD_FIFO_OVERRUN)
		return -EOVERFLOW;
	if (mac->interrupt_status != result.accepted_trigger_status)
		return -EUCLEAN;

	return 0;
}

static void capture_gpon_post_pop(struct o3_rx_context *ctx,
				  struct gpon_mac_snapshot *s,
				  u64 start_ns)
{
	s->ploamd_fifo_status =
		ioread32(ctx->gpon + GPON_G_PLOAMD_FIFO_STS);
	s->interrupt_status = ioread32(ctx->gpon + GPON_G_INT_STATUS);
	s->interrupt_enable = ioread32(ctx->gpon + GPON_G_INT_ENABLE);
	s->activation_status =
		ioread32(ctx->gpon + GPON_G_ACTIVATION_ST);
	s->tx_burst_count = ioread32(ctx->gpon + GPON_DBG_TX_BST_CNT);
	o3_rx_capture_mac_tx_gem(ctx, &s->tx_correlation);
	s->tx_gem_valid = true;
	s->onu_id = ioread32(ctx->gpon + GPON_G_ONU_ID);
	s->global_config = ioread32(ctx->gpon + GPON_G_GBL_CFG);
	s->vendor_id = ioread32(ctx->gpon + GPON_G_VENDOR_ID);
	s->vs_sn = ioread32(ctx->gpon + GPON_G_VS_SN);
	s->sn_msg_cfg = ioread32(ctx->gpon + GPON_G_SN_MSG_CFG);
	s->ploamu_fifo_status =
		ioread32(ctx->gpon + GPON_G_PLOAMU_FIFO_STS);
	s->o3_o4_ploamu_control =
		ioread32(ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
	s->elapsed_ns = ktime_get_ns() - start_ns;
	result.gpon_reads += 12;
}

static void capture_gpon_pre_pop(struct o3_rx_context *ctx,
				 struct gpon_mac_snapshot *s,
				 u64 start_ns)
{
	s->activation_status =
		ioread32(ctx->gpon + GPON_G_ACTIVATION_ST);
	s->ploamd_fifo_status =
		ioread32(ctx->gpon + GPON_G_PLOAMD_FIFO_STS);
	s->interrupt_status = ioread32(ctx->gpon + GPON_G_INT_STATUS);
	s->interrupt_enable = ioread32(ctx->gpon + GPON_G_INT_ENABLE);
	s->tx_burst_count = ioread32(ctx->gpon + GPON_DBG_TX_BST_CNT);
	o3_rx_capture_mac_tx_gem(ctx, &s->tx_correlation);
	s->tx_gem_valid = true;
	s->onu_id = ioread32(ctx->gpon + GPON_G_ONU_ID);
	s->global_config = ioread32(ctx->gpon + GPON_G_GBL_CFG);
	s->vendor_id = ioread32(ctx->gpon + GPON_G_VENDOR_ID);
	s->vs_sn = ioread32(ctx->gpon + GPON_G_VS_SN);
	s->sn_msg_cfg = ioread32(ctx->gpon + GPON_G_SN_MSG_CFG);
	s->ploamu_fifo_status =
		ioread32(ctx->gpon + GPON_G_PLOAMU_FIFO_STS);
	s->o3_o4_ploamu_control =
		ioread32(ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
	s->elapsed_ns = ktime_get_ns() - start_ns;
	result.gpon_reads += 12;
}

static int verify_pop_post(const struct gpon_mac_snapshot *mac,
			   u32 expected_fifo_status)
{
	u32 upstream_sources = o3_rx_upstream_activity_sources(mac);

	o3_rx_latch_upstream_activity(upstream_sources);
	if (upstream_sources) {
		o3_rx_set_upstream_stop_reason(upstream_sources);
		return -EPERM;
	}
	if (mac->ploamd_fifo_status & PLOAMD_FIFO_OVERRUN)
		return -EOVERFLOW;
	if (mac->ploamd_fifo_status != expected_fifo_status)
		return -ENODATA;
	if (mac->activation_status != result.activation_before ||
	    mac->interrupt_enable ||
	    mac->interrupt_status != result.accepted_trigger_status ||
	    mac->interrupt_status & INT_TX_ACTIVITY ||
	    mac->tx_burst_count != result.mac_before.tx_burst_count)
		return -EPERM;
	if (!o3_rx_snapshot_matches(mac, false) ||
	    mac->global_config != result.mac_before.global_config ||
	    !o3_rx_ploamu_control_matches(mac->o3_o4_ploamu_control) ||
	    mac->ploamu_fifo_status != result.mac_before.ploamu_fifo_status)
		return -EUCLEAN;

	return 0;
}

static int verify_second_pre_pop_boundary(void)
{
	if (result.second_pre_pop_status & INT_TX_ACTIVITY)
		o3_rx_latch_upstream_activity(
			O3_RX_UPSTREAM_LATCH_TX_STATUS);
	if (result.second_pre_pop_fifo_status & PLOAMD_FIFO_OVERRUN)
		return -EOVERFLOW;
	if (result.second_pre_pop_fifo_status !=
	    O3_RX_FIFO_AFTER_FIRST_POP)
		return -ENODATA;
	if (result.second_pre_pop_status != result.accepted_trigger_status)
		return -EUCLEAN;

	return 0;
}

/* Runs only inside the caller's IRQ-disabled O1 window. */
static void o3_rx_capture_second_record(struct o3_rx_context *ctx,
					  u64 hold_start, u64 section_start_ns,
					  u64 *second_pop_start_ns,
					  u64 *second_pop_end_ns)
{
	struct o3_rx_tx_correlation_snapshot tx_correlation;
	u32 upstream_sources;

	/* Re-prove the exact 9-to-6 boundary immediately before record1. */
	result.second_pre_pop_fifo_status =
		ioread32(ctx->gpon + GPON_G_PLOAMD_FIFO_STS);
	result.second_pre_pop_status = ioread32(ctx->gpon + GPON_G_INT_STATUS);
	o3_rx_capture_mac_tx_gem(ctx, &tx_correlation);
	upstream_sources =
		o3_rx_tx_correlation_upstream_sources(&tx_correlation);
	o3_rx_latch_upstream_activity(upstream_sources);
	result.second_pre_pop_valid = true;
	result.gpon_reads += 2;
	if (upstream_sources) {
		o3_rx_set_upstream_stop_reason(upstream_sources);
		result.second_pre_pop_result = -EPERM;
		return;
	}
	result.second_pre_pop_result = verify_second_pre_pop_boundary();
	if (result.second_pre_pop_result) {
		result.stop_reason = O3_RX_REASON_FIRST_POP_BOUNDARY;
		return;
	}
	if (ktime_get_ns() - section_start_ns >=
	    (u64)O3_RX_FIFO_POP_HARD_US * NSEC_PER_USEC) {
		result.fifo_pop_limit_result = -ETIME;
		return;
	}

	*second_pop_start_ns = ktime_get_ns();
	/* Exactly the second OEM-sized record; there is no third pop. */
	result.ploamd_words[1][0] =
		ioread32(ctx->gpon + GPON_G_PLOAMD_RDATA);
	result.ploamd_words[1][1] =
		ioread32(ctx->gpon + GPON_G_PLOAMD_RDATA);
	result.ploamd_words[1][2] =
		ioread32(ctx->gpon + GPON_G_PLOAMD_RDATA);
	result.second_record_captured = true;
	result.fifo_data_reads = O3_RX_PLOAMD_TOTAL_WORDS;
	*second_pop_end_ns = ktime_get_ns();
	capture_gpon_post_pop(ctx, &result.fifo_post, hold_start);
	result.fifo_post_valid = true;
	capture_xpon_guard(ctx, &result.guard_post_pop);
	result.guard_post_pop_valid = true;
}

/* Runs only inside the caller's IRQ-disabled O1 window. */
static void o3_rx_capture_first_record(struct o3_rx_context *ctx,
					 u64 hold_start, u64 section_start_ns,
					 u64 *first_pop_start_ns,
					 u64 *first_pop_end_ns,
					 u64 *second_pop_start_ns,
					 u64 *second_pop_end_ns)
{
	*first_pop_start_ns = ktime_get_ns();
	/* These and record1 below are the only six RDATA accesses. */
	result.ploamd_words[0][0] =
		ioread32(ctx->gpon + GPON_G_PLOAMD_RDATA);
	result.ploamd_words[0][1] =
		ioread32(ctx->gpon + GPON_G_PLOAMD_RDATA);
	result.ploamd_words[0][2] =
		ioread32(ctx->gpon + GPON_G_PLOAMD_RDATA);
	result.first_record_captured = true;
	result.fifo_data_reads = O3_RX_PLOAMD_WORDS_PER_RECORD;
	*first_pop_end_ns = ktime_get_ns();

	/* FIFO/status are first in this full post-record0 MAC snapshot. */
	capture_gpon_post_pop(ctx, &result.fifo_after_first, hold_start);
	result.fifo_after_first_valid = true;
	capture_xpon_guard(ctx, &result.guard_after_first);
	result.guard_after_first_valid = true;
	result.first_pop_post_result =
		verify_pop_post(&result.fifo_after_first,
				O3_RX_FIFO_AFTER_FIRST_POP);
	result.guard_after_first_result =
		verify_xpon_guard(&result.guard_after_first);
	if (result.first_pop_post_result || result.guard_after_first_result) {
		if (!result.upstream_activity_latched)
			result.stop_reason = O3_RX_REASON_FIRST_POP_BOUNDARY;
		return;
	}
	if (ktime_get_ns() - section_start_ns >=
	    (u64)O3_RX_FIFO_POP_HARD_US * NSEC_PER_USEC) {
		result.fifo_pop_limit_result = -ETIME;
		return;
	}

	o3_rx_capture_second_record(ctx, hold_start, section_start_ns,
				      second_pop_start_ns, second_pop_end_ns);
}

static void o3_rx_restore_o1_two_pop(struct o3_rx_context *ctx,
				       u64 hold_start, bool allow_pop)
{
	struct xpon_guard_snapshot prewrite_guard;
	struct o3_rx_tx_correlation_snapshot tx_correlation;
	unsigned long flags;
	u64 section_start_ns;
	u64 trigger_ns = hold_start + result.downstream_trigger.elapsed_ns;
	u64 o1_write_ns;
	u64 first_pop_start_ns = 0;
	u64 first_pop_end_ns = 0;
	u64 second_pop_start_ns = 0;
	u64 second_pop_end_ns = 0;
	u64 section_end_ns;
	u32 ploamu_control;
	u32 upstream_sources;
	int err;

	preempt_disable();
	local_irq_save(flags);
	section_start_ns = ktime_get_ns();
	/*
	 * The accepted trigger was checked against a full MAC/physical snapshot in
	 * the preceding poll.  Keep this immediate pre-write guard non-sleeping so
	 * the bounded 9->6->3 FIFO window is not delayed by platform lookups.
	 * Phase28 is permanently pinned and is re-verified after the O1 window.
	 */
	capture_xpon_guard(ctx, &prewrite_guard);
	ploamu_control =
		ioread32(ctx->gpon + GPON_O3_O4_PLOAMU_CTRL);
	o3_rx_capture_mac_tx_gem(ctx, &tx_correlation);
	upstream_sources =
		o3_rx_tx_correlation_upstream_sources(&tx_correlation);
	o3_rx_latch_upstream_activity(upstream_sources);
	result.gpon_reads++;
	result.o1_restore_pre_guard_valid = true;
	result.restore_guard_checks++;
	err = verify_xpon_guard(&prewrite_guard);
	if (upstream_sources) {
		o3_rx_set_upstream_stop_reason(upstream_sources);
		if (!err)
			err = -EPERM;
	}
	if (!err && !o3_rx_ploamu_control_matches(ploamu_control)) {
		result.stop_reason =
			O3_RX_REASON_PLOAMU_CTRL_INVARIANT;
		err = -EUCLEAN;
	}
	if (err) {
		result.restore_guard_failures++;
		if (!result.o1_restore_pre_guard_result)
			result.o1_restore_pre_guard_result = err;
		result.restore_unsafe = true;
		allow_pop = false;
	}
	o1_write_ns = ktime_get_ns();
	iowrite32(result.activation_before,
		  ctx->gpon + GPON_G_ACTIVATION_ST);
	result.restore_write_attempts++;
	capture_gpon_pre_pop(ctx, &result.mac_after, hold_start);
	result.mac_after_valid = true;
	capture_xpon_guard(ctx, &result.guard_pre_pop);
	result.guard_pre_pop_valid = true;
	result.activation_after = result.mac_after.activation_status;
	result.activation_restore_result =
		result.activation_after == result.activation_before ?
		0 : -EIO;
	if (result.activation_restore_result)
		result.restore_readback_failures++;
	result.mac_after_result = verify_mac_after(&result.mac_after);
	result.guard_pre_pop_result =
		verify_xpon_guard(&result.guard_pre_pop);
	if (allow_pop && result.guard_pre_pop_result)
		result.pre_pop_result = result.guard_pre_pop_result;

	if (allow_pop && !result.activation_restore_result &&
	    !result.mac_after_result &&
	    !result.guard_pre_pop_result) {
		result.pre_pop_result =
			verify_o1_pop_boundary(&result.mac_after);
		if (!result.pre_pop_result) {
			/*
			 * The first trigger snapshot is intentionally not trusted as
			 * the final pop boundary: its status and FIFO reads are
			 * separated by other read-only registers.  Re-read the exact
			 * pair immediately before the only destructive accesses.
			 */
			result.final_pre_pop_fifo_status =
				ioread32(ctx->gpon + GPON_G_PLOAMD_FIFO_STS);
			result.final_pre_pop_status =
				ioread32(ctx->gpon + GPON_G_INT_STATUS);
			o3_rx_capture_mac_tx_gem(ctx, &tx_correlation);
			upstream_sources =
				o3_rx_tx_correlation_upstream_sources(
					&tx_correlation);
			o3_rx_latch_upstream_activity(upstream_sources);
			result.final_pre_pop_valid = true;
			result.gpon_reads += 2;
			if (upstream_sources) {
				o3_rx_set_upstream_stop_reason(upstream_sources);
				result.pre_pop_result = -EPERM;
			}
			if (result.final_pre_pop_status & INT_TX_ACTIVITY)
				o3_rx_latch_upstream_activity(
					O3_RX_UPSTREAM_LATCH_TX_STATUS);
			if (!result.pre_pop_result &&
			    (result.final_pre_pop_fifo_status !=
				    O3_RX_FIFO_BEFORE_POP ||
			    result.final_pre_pop_status !=
				    result.accepted_trigger_status)) {
				result.pre_pop_result = -EUCLEAN;
				result.stop_reason =
					O3_RX_REASON_FINAL_PRE_POP_CHANGED;
			}
		}
		if (!result.pre_pop_result) {
			result.trigger_to_o1_ns = o1_write_ns >= trigger_ns ?
				o1_write_ns - trigger_ns : U64_MAX;
			if (result.trigger_to_o1_ns >=
			    (u64)O3_RX_FIFO_POP_HARD_US * NSEC_PER_USEC) {
				result.fifo_pop_limit_result = -ETIME;
			} else {
				result.fifo_pop_attempted = true;
				o3_rx_capture_first_record(ctx, hold_start,
							     section_start_ns,
							     &first_pop_start_ns,
							     &first_pop_end_ns,
							     &second_pop_start_ns,
							     &second_pop_end_ns);
			}
		}
	}
	section_end_ns = ktime_get_ns();
	local_irq_restore(flags);
	preempt_enable();

	result.gpon_writes++;
	result.o1_restore_post_guard_valid = true;
	result.restore_guard_checks++;
	result.o1_restore_post_guard_result = o3_rx_restored_o1_guard(ctx);
	if (result.o1_restore_post_guard_result) {
		result.restore_guard_failures++;
		result.restore_unsafe = true;
	}
	result.irq_off_ns = section_end_ns - section_start_ns;
	result.hold_ns = o1_write_ns - hold_start;
	result.terminal_gap_ns = result.checks ?
		result.hold_ns - result.last_check_elapsed_ns :
		result.hold_ns;
	result.fifo_pop_ns = first_pop_start_ns ?
		section_end_ns - first_pop_start_ns : 0;
	result.first_pop_ns = first_pop_end_ns ?
		first_pop_end_ns - first_pop_start_ns : 0;
	result.second_pop_ns = second_pop_end_ns ?
		second_pop_end_ns - second_pop_start_ns : 0;
	result.trigger_to_post_ns =
		result.first_record_captured && section_end_ns >= trigger_ns ?
		section_end_ns - trigger_ns : 0;
	if (result.first_record_captured) {
		result.gpon_reads += result.fifo_data_reads;
		if (result.irq_off_ns >=
		    (u64)O3_RX_FIFO_POP_HARD_US * NSEC_PER_USEC ||
		    result.trigger_to_post_ns >=
		    (u64)O3_RX_FIFO_POP_HARD_US * NSEC_PER_USEC)
			result.fifo_pop_limit_result = -ETIME;
	}
}

static int o3_rx_poll_once(struct o3_rx_context *ctx, u64 start_ns,
			     struct gpon_mac_snapshot *mac)
{
	struct xpon_guard_snapshot guard;
	u32 live_wan;
	int err;

	capture_gpon_probe(ctx, mac, start_ns);
	err = regmap_read(ctx->scu, EN751221_SCU_WAN_CONF, &live_wan);
	if (err)
		return err;
	if (live_wan != result.wan_gpon) {
		result.wan_mismatch = live_wan;
		result.stop_reason = O3_RX_REASON_MUX_CHANGED;
		return -EUCLEAN;
	}
	capture_xpon_guard(ctx, &guard);
	return check_observation(mac, &guard);
}

static int o3_rx_checkpoint_once(struct o3_rx_context *ctx, u64 start_ns,
				   struct gpon_mac_snapshot *mac)
{
	struct xpon_guard_snapshot guard;
	u32 live_wan;
	int err;

	capture_gpon_checkpoint(ctx, mac, start_ns);
	err = regmap_read(ctx->scu, EN751221_SCU_WAN_CONF, &live_wan);
	if (err)
		return err;
	if (live_wan != result.wan_gpon) {
		result.wan_mismatch = live_wan;
		result.stop_reason = O3_RX_REASON_MUX_CHANGED;
		return -EUCLEAN;
	}
	capture_xpon_guard(ctx, &guard);
	return check_observation(mac, &guard);
}

/*
 * Observe sticky startup IRQ status without acknowledging or clearing it.
 * The passive O3 lab must have zero writes to GPON_G_INT_STATUS.
 */
static int o3_rx_sample_interrupt_status(struct o3_rx_context *ctx,
					 u32 *pre_status, u32 *post_status,
					 u64 *latency_ns)
{
	unsigned long flags;
	u64 start_ns;
	u64 end_ns;

	local_irq_save(flags);
	start_ns = ktime_get_ns();
	*pre_status = ioread32(ctx->gpon + GPON_G_INT_STATUS);
	*post_status = *pre_status;
	end_ns = ktime_get_ns();
	local_irq_restore(flags);

	result.gpon_reads++;
	*latency_ns = end_ns - start_ns;
	if (*latency_ns >=
	    (u64)GPON_POLL_HARD_GAP_US * NSEC_PER_USEC) {
		result.stop_reason =
			O3_RX_REASON_POLL_GAP;
		return -ETIME;
	}

	return 0;
}

static int o3_rx_run_o3_lab(struct o3_rx_context *ctx)
{
	struct o3_rx_upstream_overhead overhead;
	int err = 0;
	int restore_err;

	result.initial_records_result =
		o3_rx_validate_initial_records(&overhead);
	if (result.initial_records_result) {
		result.stop_reason = O3_RX_REASON_INITIAL_RECORDS_INVALID;
		return result.initial_records_result;
	}

	result.formatter_program_result =
		o3_rx_program_initial_formatter(ctx, &overhead);
	if (result.formatter_program_result) {
		result.stop_reason = O3_RX_REASON_FORMATTER_WRITE;
		err = result.formatter_program_result;
		goto restore_formatter;
	}

	result.activation_o3_result = o3_rx_enter_o3(ctx);
	if (result.activation_o3_result) {
		result.stop_reason = O3_RX_REASON_O3_ACTIVATION;
		err = result.activation_o3_result;
		goto restore_formatter;
	}

	result.o3_observation_result = o3_rx_observe_o3(ctx);
	err = result.o3_observation_result;

restore_formatter:
	restore_err = o3_rx_restore_formatter(ctx);
	result.formatter_restore_result = restore_err;
	if (restore_err) {
		if (!err) {
			result.stop_reason = O3_RX_REASON_FORMATTER_RESTORE;
			err = restore_err;
		}
		result.restore_unsafe = true;
	}

	return err;
}

static int o3_rx_run(struct o3_rx_context *ctx)
{
	struct gpon_mac_snapshot probe = {};
	u32 original_mode;
	u64 first_gtc_deadline_ns;
	u64 stage_a_deadline_ns;
	u64 total_deadline_ns;
	u64 cycle_start, hold_start;
	int check;
	int err;

	err = o3_rx_phase28_xpon_guard(ctx, &result.guard_before);
	if (err) {
		result.stop_reason = O3_RX_REASON_PREFLIGHT_XPON_GUARD;
		return err;
	}

	err = regmap_read(ctx->scu, EN751221_SCU_WAN_CONF,
			  &result.wan_before);
	if (err) {
		result.stop_reason = O3_RX_REASON_PREFLIGHT_SCU_READ;
		return err;
	}
	err = regmap_read(ctx->scu, EN751221_SCU_RESET_CTRL2,
			  &result.reset_ctrl2);
	if (err) {
		result.stop_reason = O3_RX_REASON_PREFLIGHT_SCU_READ;
		return err;
	}
	err = regmap_read(ctx->scu, EN751221_SCU_RESET_CTRL1,
			  &result.reset_ctrl1);
	if (err) {
		result.stop_reason = O3_RX_REASON_PREFLIGHT_SCU_READ;
		return err;
	}

	original_mode = FIELD_GET(WAN_MODE, result.wan_before);
	if (original_mode != WAN_MODE_ATM) {
		result.stop_reason = O3_RX_REASON_PREFLIGHT_WAN_MODE;
		return -EUCLEAN;
	}
	if ((result.reset_ctrl2 & RESET2_XPON_PHY) ||
	    (result.reset_ctrl1 & REQUIRED_RELEASED_RESET1)) {
		result.stop_reason = O3_RX_REASON_PREFLIGHT_RESET_STATE;
		return -EBUSY;
	}
	result.preflight_passed = true;

	/* Immediate physical/Phase28 proof before changing the shared WAN mux. */
	err = o3_rx_phase28_xpon_guard(ctx, NULL);
	if (err) {
		result.stop_reason = O3_RX_REASON_PREFLIGHT_XPON_GUARD;
		return err;
	}
	cycle_start = ktime_get_ns();
	result.update_attempted = true;
	result.scu_update_calls++;
	err = regmap_update_bits(ctx->scu, EN751221_SCU_WAN_CONF, WAN_MODE,
				 WAN_MODE_GPON);
	if (err) {
		result.stop_reason = O3_RX_REASON_MUX_SELECT_WRITE;
		goto restore_mode;
	}

	err = regmap_read(ctx->scu, EN751221_SCU_WAN_CONF,
			  &result.wan_gpon);
	if (err || FIELD_GET(WAN_MODE, result.wan_gpon) != WAN_MODE_GPON) {
		result.stop_reason = O3_RX_REASON_MUX_SELECT_READBACK;
		if (!err)
			err = -EIO;
		goto restore_mode;
	}

	/* Re-prove the receive-only/Phase28 boundary after the shared mux write. */
	err = o3_rx_phase28_xpon_guard(ctx, &result.guard_before);
	if (err) {
		result.stop_reason = O3_RX_REASON_PRE_OBSERVATION_GUARD;
		goto restore_mode;
	}

	capture_gpon_checkpoint(ctx, &result.mac_before, cycle_start);
	result.mac_before_valid = true;
	/* Keep factory identity words private; retain only invariant results. */
	result.identity_snapshot_unchanged = true;
	result.onu_id_never_valid =
		!(result.mac_before.onu_id & ONU_ID_VALID);
	result.ploamu_control_before =
		result.mac_before.o3_o4_ploamu_control;
	result.ploamu_control_forced =
		result.ploamu_control_before | O3_O4_PLOAMU_CTRL_SW;
	result.ploamu_control_baseline_hw_auto =
		!(result.ploamu_control_before & O3_O4_PLOAMU_CTRL_SW);
	result.activation_before = result.mac_before.activation_status;
	result.baseline_status = result.mac_before.interrupt_status;
	result.baseline_unsafe_mask =
		result.baseline_status & ~O3_RX_SAFE_STARTUP_MASK;
	result.baseline_rx_gtc = result.mac_before.rx_gtc_count;
	result.baseline_eof_preexisting =
		result.baseline_status & INT_RX_EOF_ERR;
	result.baseline_expected_match =
		o3_rx_baseline_status_allowed(result.baseline_status);
	if (FIELD_GET(ACTIVATION_STATE, result.activation_before) !=
	    ACTIVATION_O1 || result.mac_before.onu_id != RESET_ONU_ID ||
	    result.mac_before.global_config != RESET_GLOBAL_CONFIG ||
	    result.mac_before.ploamu_fifo_status != RESET_PLOAMU_FIFO ||
	    result.mac_before.interrupt_enable ||
	    result.mac_before.tx_burst_count ||
	    !result.baseline_expected_match ||
	    (result.mac_before.ploamd_fifo_status & PLOAMD_FIFO_OVERRUN) ||
	    (result.baseline_status == O3_RX_BASELINE_PRE_DS_FEC &&
	     result.baseline_rx_gtc) ||
	    (result.baseline_eof_preexisting && !result.baseline_rx_gtc) ||
	    FIELD_GET(PLOAM_FIFO_LEVEL,
		      result.mac_before.ploamd_fifo_status)) {
		result.stop_reason = O3_RX_REASON_PRE_OBSERVATION_PRECONDITION;
		err = -EUCLEAN;
		goto restore_mode;
	}

	/*
	 * Validate physical TX_DISABLE and every static baseline invariant before
	 * either rejecting autonomous control or performing the one-bit RMW.
	 */
	err = o3_rx_static_guard(ctx, ACTIVATION_O1);
	if (err) {
		result.stop_reason = O3_RX_REASON_PRE_OBSERVATION_GUARD;
		goto restore_mode;
	}
	if (result.ploamu_control_baseline_hw_auto) {
		if (!result.ploamu_control_force_requested) {
			result.stop_reason =
				O3_RX_REASON_O3_O4_PLOAMU_HW_AUTO;
			err = -EPERM;
			goto restore_mode;
		}
		result.ploamu_control_program_result =
			o3_rx_force_software_ploamu_control(ctx);
		if (result.ploamu_control_program_result) {
			result.stop_reason =
				O3_RX_REASON_PLOAMU_CTRL_FORCE;
			err = result.ploamu_control_program_result;
			goto restore_mode;
		}
	}

	/* Immediate full invariant proof before entering O2. */
	err = o3_rx_static_guard(ctx, ACTIVATION_O1);
	if (err) {
		result.stop_reason = O3_RX_REASON_PRE_OBSERVATION_GUARD;
		goto restore_mode;
	}
	result.activation_write_attempted = true;
	hold_start = ktime_get_ns();
	iowrite32((result.activation_before & ~ACTIVATION_STATE) |
		  ACTIVATION_O2, ctx->gpon + GPON_G_ACTIVATION_ST);
	result.gpon_writes++;
	result.activation_o2 =
		ioread32(ctx->gpon + GPON_G_ACTIVATION_ST);
	result.gpon_reads++;
	if (FIELD_GET(ACTIVATION_STATE, result.activation_o2) !=
	    ACTIVATION_O2) {
		result.stop_reason = O3_RX_REASON_ACTIVATION_READBACK;
		err = -EIO;
		goto restore_activation;
	}
	err = o3_rx_guard_common(ctx, ACTIVATION_O2, true);
	if (err) {
		result.stop_reason = O3_RX_REASON_PRE_OBSERVATION_GUARD;
		goto restore_activation;
	}

	first_gtc_deadline_ns = hold_start +
				(u64)GPON_FIRST_GTC_TIMEOUT_US *
				NSEC_PER_USEC;
	for (;;) {
		if (kthread_should_stop()) {
			result.stop_reason =
				O3_RX_REASON_ABORT_REQUESTED;
			err = -EINTR;
			goto restore_activation;
		}
		check = o3_rx_poll_once(ctx, hold_start, &probe);
		result.first_gtc_polls++;
		if (check) {
			err = check;
			goto restore_activation;
		}
		check = check_interrupt_status(&probe,
					       O3_RX_REASON_STARTUP_STATUS_UNSAFE);
		if (check < 0) {
			err = check;
			goto restore_activation;
		}
		if (check > 0) {
			err = o3_rx_arm_single_pop();
			goto restore_activation;
		}
		if (probe.rx_gtc_count != result.mac_before.rx_gtc_count) {
			result.first_gtc = probe;
			result.first_gtc_valid = true;
			result.first_gtc_elapsed_ns = probe.elapsed_ns;
			result.startup_status = probe.interrupt_status;
			result.startup_unsafe_mask =
				probe.interrupt_status &
				~O3_RX_SAFE_STARTUP_MASK;
			result.startup_expected_match =
				probe.interrupt_status ==
				O3_RX_EXPECTED_STARTUP_MASK;
			break;
		}
		if (ktime_get_ns() >= first_gtc_deadline_ns) {
			result.stop_reason =
				O3_RX_REASON_FIRST_GTC_TIMEOUT;
			err = -ETIMEDOUT;
			goto restore_activation;
		}
		usleep_range(GPON_FIRST_POLL_MIN_US,
			     GPON_FIRST_POLL_MAX_US);
	}

	if (!result.startup_expected_match) {
		result.stop_reason = O3_RX_REASON_STARTUP_STATUS_UNEXPECTED;
		err = -EUCLEAN;
		goto restore_activation;
	}

	if (!(result.startup_status & INT_RX_EOF_ERR)) {
		result.stop_reason = O3_RX_REASON_STARTUP_EOF_MISSING;
		err = -EUCLEAN;
		goto restore_activation;
	}

	/*
	 * Passive stage A samples the sticky startup word without acknowledging it.
	 * Keeping the complete startup word latched makes the later PLOAMD trigger
	 * exact and leaves GPON_G_INT_STATUS strictly read-only in this lab.
	 */
	result.stage_a_clear_elapsed_ns =
		ktime_get_ns() - hold_start;
	check = o3_rx_sample_interrupt_status(ctx, &result.stage_a_pre_status,
					       &result.stage_a_post_status,
					       &result.stage_a_readback_latency_ns);
	if (check) {
		err = check;
		goto restore_activation;
	}
	result.stage_a_passive_sampled = true;
	if (result.stage_a_pre_status != O3_RX_PASSIVE_IDLE_STATUS ||
	    result.stage_a_post_status != O3_RX_PASSIVE_IDLE_STATUS) {
		result.stop_reason = O3_RX_REASON_STARTUP_STATUS_UNEXPECTED;
		err = -EUCLEAN;
		goto restore_activation;
	}
	result.stage_a_non_target_lost =
		result.stage_a_pre_status &
		~result.stage_a_post_status;
	if (result.stage_a_non_target_lost) {
		result.stop_reason =
			O3_RX_REASON_STAGE_A_NON_TARGET_LOST;
		err = -EIO;
		goto restore_activation;
	}

	check = o3_rx_poll_once(ctx, hold_start, &probe);
	result.stage_a_polls++;
	if (check) {
		err = check;
		goto restore_activation;
	}
	result.stage_a_readback = probe;
	result.stage_a_readback_valid = true;
	check = check_interrupt_status(&probe,
				       O3_RX_REASON_UNSAFE_INTERRUPT_STATUS);
	if (check < 0) {
		err = check;
		goto restore_activation;
	}
	if (check > 0) {
		err = o3_rx_arm_single_pop();
		goto restore_activation;
	}
	if (probe.interrupt_status & INT_RX_EOF_ERR) {
		result.first_eof_reassert = probe;
		result.first_eof_reassert_valid = true;
		result.first_eof_reassert_latency_ns =
			probe.elapsed_ns -
			result.stage_a_clear_elapsed_ns;
	}

	stage_a_deadline_ns = hold_start +
			      result.stage_a_clear_elapsed_ns +
			      (u64)GPON_PASSIVE_STARTUP_SETTLE_US * NSEC_PER_USEC;
	for (;;) {
		if (kthread_should_stop()) {
			result.stop_reason =
				O3_RX_REASON_ABORT_REQUESTED;
			err = -EINTR;
			goto restore_activation;
		}
		if (ktime_get_ns() >= stage_a_deadline_ns)
			break;
		usleep_range(GPON_POLL_MIN_US, GPON_POLL_MAX_US);
		check = o3_rx_poll_once(ctx, hold_start, &probe);
		result.stage_a_polls++;
		if (check) {
			err = check;
			goto restore_activation;
		}
		check = check_interrupt_status(&probe,
					       O3_RX_REASON_UNSAFE_INTERRUPT_STATUS);
		if (check < 0) {
			err = check;
			goto restore_activation;
		}
		if (check > 0) {
			err = o3_rx_arm_single_pop();
			goto restore_activation;
		}
		if (!result.first_eof_reassert_valid &&
		    probe.interrupt_status & INT_RX_EOF_ERR) {
			result.first_eof_reassert = probe;
			result.first_eof_reassert_valid = true;
			result.first_eof_reassert_latency_ns =
				probe.elapsed_ns -
				result.stage_a_clear_elapsed_ns;
		}
	}

	check = o3_rx_checkpoint_once(ctx, hold_start,
					&result.stage_a_end);
	result.stage_a_polls++;
	if (check) {
		err = check;
		goto restore_activation;
	}
	result.stage_a_end_valid = true;
	result.stage_a_end_elapsed_ns = result.stage_a_end.elapsed_ns;
	check = check_interrupt_status(&result.stage_a_end,
				       O3_RX_REASON_UNSAFE_INTERRUPT_STATUS);
	if (check < 0) {
		err = check;
		goto restore_activation;
	}
	if (check > 0) {
		err = o3_rx_arm_single_pop();
		goto restore_activation;
	}
	if (!result.first_eof_reassert_valid &&
	    result.stage_a_end.interrupt_status & INT_RX_EOF_ERR) {
		result.first_eof_reassert = result.stage_a_end;
		result.first_eof_reassert_valid = true;
		result.first_eof_reassert_latency_ns =
			result.stage_a_end.elapsed_ns -
			result.stage_a_clear_elapsed_ns;
	}

	/* Passive stage B is a second exact read-only startup observation. */
	result.stage_b_clear_elapsed_ns =
		ktime_get_ns() - hold_start;
	check = o3_rx_sample_interrupt_status(ctx, &result.stage_b_pre_status,
					       &result.stage_b_post_status,
					       &result.stage_b_readback_latency_ns);
	if (check) {
		err = check;
		goto restore_activation;
	}
	result.stage_b_passive_sampled = true;
	result.stage_b_expected_match =
		result.stage_b_pre_status == O3_RX_PASSIVE_IDLE_STATUS &&
		result.stage_b_post_status == O3_RX_PASSIVE_IDLE_STATUS;
	if (!result.stage_b_expected_match) {
		result.stop_reason =
			O3_RX_REASON_STAGE_B_TARGET_UNEXPECTED;
		err = -EUCLEAN;
		goto restore_activation;
	}
	result.stage_b_non_target_lost =
		result.stage_b_pre_status &
		~result.stage_b_post_status;
	if (result.stage_b_non_target_lost) {
		result.stop_reason =
			O3_RX_REASON_STAGE_B_NON_TARGET_LOST;
		err = -EIO;
		goto restore_activation;
	}

	check = o3_rx_poll_once(ctx, hold_start,
				  &result.stage_b_readback);
	result.stage_b_polls++;
	if (check) {
		err = check;
		goto restore_activation;
	}
	result.stage_b_readback_valid = true;
	check = check_interrupt_status(&result.stage_b_readback,
				       O3_RX_REASON_UNSAFE_INTERRUPT_STATUS);
	if (check < 0) {
		err = check;
		goto restore_activation;
	}
	if (check > 0) {
		err = o3_rx_arm_single_pop();
		goto restore_activation;
	}
	total_deadline_ns = hold_start +
			    (u64)GPON_TOTAL_OBSERVE_US *
			    NSEC_PER_USEC;
	for (;;) {
		if (kthread_should_stop()) {
			result.stop_reason =
				O3_RX_REASON_ABORT_REQUESTED;
			err = -EINTR;
			goto restore_activation;
		}
		if (ktime_get_ns() >= total_deadline_ns)
			break;
		usleep_range(GPON_POLL_MIN_US, GPON_POLL_MAX_US);
		check = o3_rx_poll_once(ctx, hold_start, &probe);
		result.stage_b_polls++;
		if (check) {
			err = check;
			goto restore_activation;
		}
		check = check_interrupt_status(&probe,
					       O3_RX_REASON_UNSAFE_INTERRUPT_STATUS);
		if (check < 0) {
			err = check;
			goto restore_activation;
		}
		if (check > 0) {
			err = o3_rx_arm_single_pop();
			goto restore_activation;
		}
		if (!result.first_eof_reassert_valid &&
		    probe.interrupt_status & INT_RX_EOF_ERR) {
			result.first_eof_reassert = probe;
			result.first_eof_reassert_valid = true;
			result.first_eof_reassert_latency_ns =
				probe.elapsed_ns -
				result.stage_a_clear_elapsed_ns;
		}
	}

	check = o3_rx_checkpoint_once(ctx, hold_start,
					&result.observation_end);
	result.stage_b_polls++;
	if (check) {
		err = check;
		goto restore_activation;
	}
	result.observation_end_valid = true;
	check = check_interrupt_status(&result.observation_end,
				       O3_RX_REASON_UNSAFE_INTERRUPT_STATUS);
	if (check < 0) {
		err = check;
		goto restore_activation;
	}
	if (check > 0) {
		err = o3_rx_arm_single_pop();
		goto restore_activation;
	}
	result.stop_reason = O3_RX_REASON_TIMELINE_COMPLETE;
	err = -ENODATA;

restore_activation:
	if (result.activation_write_attempted) {
		bool allow_pop = result.fifo_pop_armed && !err;
		int boundary_err;

		if (allow_pop) {
			boundary_err =
				regmap_read(ctx->scu,
					    EN751221_SCU_WAN_CONF,
					    &result.wan_pre_pop);
			if (boundary_err ||
			    result.wan_pre_pop != result.wan_gpon) {
				result.pre_pop_result =
					boundary_err ?: -EUCLEAN;
				result.stop_reason =
					O3_RX_REASON_PRE_POP_MUX_CHANGED;
				err = result.pre_pop_result;
				allow_pop = false;
			}
		}

		/*
		 * This one bounded IRQ-off window performs exact O1
		 * restoration, its readback and strict precheck, at most two
		 * fixed three-word destructive RDATA pops, and the immediate
		 * first-boundary/final-post checks.
		 * It contains no sleeping, regmap, formatting or parser call.
		 */
		o3_rx_restore_o1_two_pop(ctx, hold_start, allow_pop);
		if (result.second_record_captured) {
			boundary_err = regmap_read(ctx->scu,
						   EN751221_SCU_WAN_CONF,
						   &result.wan_post_pop);
			result.wan_post_pop_valid = !boundary_err;
			result.post_pop_mux_result = boundary_err ?:
				(result.wan_post_pop == result.wan_gpon ?
				 0 : -EUCLEAN);
			if (result.post_pop_mux_result && !err)
				err = result.post_pop_mux_result;
		}
		if (result.checks)
			result.terminal_gap_ns =
				result.hold_ns -
				result.last_check_elapsed_ns;
		if (result.terminal_gap_ns > result.max_check_gap_ns)
			result.max_check_gap_ns = result.terminal_gap_ns;
		if (result.terminal_gap_ns >
		    (u64)GPON_POLL_HARD_GAP_US * NSEC_PER_USEC) {
			result.terminal_gap_result = -ETIME;
			if (result.stop_reason == O3_RX_REASON_NONE ||
			    result.stop_reason ==
			    O3_RX_REASON_TIMELINE_COMPLETE)
				result.stop_reason =
					O3_RX_REASON_POLL_GAP;
		}
		if (result.hold_ns >
		    (u64)GPON_O2_HARD_LIMIT_US * NSEC_PER_USEC)
			result.o2_limit_result = -ETIME;

		if (result.fifo_pop_armed &&
		    !result.first_record_captured) {
			if (result.guard_pre_pop_result &&
			    !result.pre_pop_result) {
				result.pre_pop_result =
					result.guard_pre_pop_result;
				result.stop_reason =
					O3_RX_REASON_PRE_POP_GUARD;
			} else if (result.fifo_pop_limit_result) {
				result.stop_reason =
					O3_RX_REASON_FIFO_POP_SLOW;
			} else if (result.pre_pop_result &&
				   result.stop_reason ==
				   O3_RX_REASON_DOWNSTREAM_PROGRESS) {
				result.stop_reason =
					O3_RX_REASON_FIFO_CHANGED_AFTER_O1;
			}
			if (!err)
				err = result.pre_pop_result ?:
				      result.fifo_pop_limit_result ?:
				      -ENODATA;
		}

		if (result.first_record_captured &&
		    !result.second_record_captured) {
			if (result.first_pop_post_result ||
			    result.guard_after_first_result ||
			    result.second_pre_pop_result) {
				if (!result.upstream_activity_latched)
					result.stop_reason =
						O3_RX_REASON_FIRST_POP_BOUNDARY;
			} else if (result.fifo_pop_limit_result) {
				result.stop_reason =
					O3_RX_REASON_FIFO_POP_SLOW;
			}
			if (!err)
				err = result.first_pop_post_result ?:
				      result.guard_after_first_result ?:
				      result.second_pre_pop_result ?:
				      result.fifo_pop_limit_result ?:
				      -ENODATA;
		}

		if (result.second_record_captured) {
			result.fifo_post_result =
				verify_pop_post(&result.fifo_post,
						O3_RX_FIFO_AFTER_SECOND_POP);
			result.guard_post_pop_result =
				verify_xpon_guard(&result.guard_post_pop);
			if (result.fifo_pop_limit_result) {
				result.stop_reason =
					O3_RX_REASON_FIFO_POP_SLOW;
				if (!err)
					err = result.fifo_pop_limit_result;
			} else if (result.fifo_post.ploamd_fifo_status !=
				   O3_RX_FIFO_AFTER_SECOND_POP) {
				result.stop_reason =
					O3_RX_REASON_FIFO_POST_STATUS;
				if (!err)
					err = result.fifo_post_result ?: -ENODATA;
			} else if (result.post_pop_mux_result) {
				result.stop_reason =
					O3_RX_REASON_POST_POP_MUX_CHANGED;
			} else if (result.fifo_post_result ||
				   result.guard_post_pop_result) {
				if (!result.upstream_activity_latched)
					result.stop_reason =
						O3_RX_REASON_POST_POP_UNSAFE;
				if (!err)
					err = result.fifo_post_result ?:
					      result.guard_post_pop_result;
			} else {
				result.records_valid = true;
				result.fifo_pop_completed = true;
				result.stop_reason = O3_RX_REASON_TWO_POP_COMPLETE;
				err = 0;
			}
		}

		if (!err && result.records_valid)
			err = o3_rx_run_o3_lab(ctx);
	}

restore_mode:
	/*
	 * Restore the saved PLOAMu-control word while the GPON aperture is still
	 * selected.  The helper refuses that write unless exact O1 and a
	 * baseline-clean TX guard are proved; a latched internal burst keeps
	 * software control until power removal.  ATM is restored regardless.
	 */
	result.ploamu_control_restore_result =
		o3_rx_restore_ploamu_control(ctx);
	if (result.ploamu_control_restore_result) {
		result.restore_unsafe = true;
		if (!err) {
			result.stop_reason =
				O3_RX_REASON_PLOAMU_CTRL_RESTORE;
			err = result.ploamu_control_restore_result;
		}
	}

	/* Always restore ATM even if the pre-restore boundary proof has failed. */
	result.restore_mode_guard_valid = true;
	result.restore_guard_checks++;
	result.restore_mode_guard_result = o3_rx_phase28_xpon_guard(ctx, NULL);
	if (result.restore_mode_guard_result) {
		result.restore_guard_failures++;
		result.restore_unsafe = true;
	}
	/*
	 * The GPON aperture is valid only while the WAN mux selects GPON.  Take
	 * the final non-invasive SN configuration snapshot before restoring ATM;
	 * do not access ctx->gpon below the mux restore.
	 */
	if (result.activation_o3_valid)
		o3_rx_note_final_sn_msg_cfg(ctx);
	result.scu_update_calls++;
	result.restore_write_attempts++;
	result.restore_result = regmap_update_bits(ctx->scu,
						   EN751221_SCU_WAN_CONF,
						   WAN_MODE, original_mode);
	result.cycle_ns = ktime_get_ns() - cycle_start;
	result.final_read_result =
		regmap_read(ctx->scu, EN751221_SCU_WAN_CONF,
			    &result.wan_after);

	result.guard_after_valid = true;
	result.restore_guard_checks++;
	result.guard_after_result =
		o3_rx_phase28_xpon_guard(ctx, &result.guard_after);
	if (result.guard_after_result)
		result.restore_guard_failures++;
	result.restore_unsafe =
		result.restore_unsafe ||
		result.activation_restore_result ||
		result.mac_after_result ||
		result.formatter_restore_result ||
		result.ploamu_control_restore_result ||
		result.ploamu_control_restore_pre_guard_result ||
		result.ploamu_control_restore_post_guard_result ||
		result.o1_restore_pre_guard_result ||
		result.o1_restore_post_guard_result ||
		result.restore_mode_guard_result ||
		result.restore_result ||
		result.final_read_result ||
		result.wan_after != result.wan_before ||
		result.guard_after_result;
	if (!err)
		err = result.ploamu_control_restore_result ?:
		      result.o1_restore_pre_guard_result ?:
		      result.o1_restore_post_guard_result ?:
		      result.restore_mode_guard_result ?:
		      result.guard_after_result;
	if (result.o1_restore_pre_guard_result)
		return result.o1_restore_pre_guard_result;
	if (result.o1_restore_post_guard_result)
		return result.o1_restore_post_guard_result;
	if (result.ploamu_control_restore_result)
		return result.ploamu_control_restore_result;
	if (result.restore_mode_guard_result)
		return result.restore_mode_guard_result;
	if (result.activation_restore_result)
		return result.activation_restore_result;
	if (result.mac_after_result)
		return result.mac_after_result;
	if (result.o2_limit_result)
		return result.o2_limit_result;
	if (result.restore_result)
		return result.restore_result;
	if (result.final_read_result)
		return result.final_read_result;
	if (result.wan_after != result.wan_before)
		return -EIO;
	if (result.terminal_gap_result)
		return result.terminal_gap_result;

	return err;
}

static const char *stop_reason_name(enum o3_rx_stop_reason reason)
{
	switch (reason) {
	case O3_RX_REASON_NONE:
		return "none";
	case O3_RX_REASON_PREFLIGHT_XPON_GUARD:
		return "preflight-xpon-guard";
	case O3_RX_REASON_PREFLIGHT_SCU_READ:
		return "preflight-scu-read";
	case O3_RX_REASON_PREFLIGHT_WAN_MODE:
		return "preflight-wan-mode";
	case O3_RX_REASON_PREFLIGHT_RESET_STATE:
		return "preflight-reset-state";
	case O3_RX_REASON_TIMELINE_COMPLETE:
		return "timeline-complete";
	case O3_RX_REASON_FIRST_GTC_TIMEOUT:
		return "first-gtc-timeout";
	case O3_RX_REASON_STARTUP_STATUS_UNSAFE:
		return "startup-status-unsafe";
	case O3_RX_REASON_STARTUP_STATUS_UNEXPECTED:
		return "startup-status-unexpected";
	case O3_RX_REASON_STARTUP_EOF_MISSING:
		return "startup-eof-missing";
	case O3_RX_REASON_STAGE_A_TARGET_NOT_CLEARED:
		return "stage-a-target-not-cleared";
	case O3_RX_REASON_STAGE_A_NON_TARGET_LOST:
		return "stage-a-non-target-lost";
	case O3_RX_REASON_STAGE_B_TARGET_UNEXPECTED:
		return "stage-b-target-unexpected";
	case O3_RX_REASON_STAGE_B_TARGET_NOT_CLEARED:
		return "stage-b-target-not-cleared";
	case O3_RX_REASON_STAGE_B_NON_TARGET_LOST:
		return "stage-b-non-target-lost";
	case O3_RX_REASON_CLEAR_READBACK_SLOW:
		return "selective-clear-readback-slow";
	case O3_RX_REASON_UNSAFE_INTERRUPT_STATUS:
		return "unsafe-interrupt-status";
	case O3_RX_REASON_ABORT_REQUESTED:
		return "abort-requested";
	case O3_RX_REASON_ACTIVATION_LEFT_O2:
		return "activation-left-o2";
	case O3_RX_REASON_INTERRUPT_ENABLED:
		return "interrupt-enabled";
	case O3_RX_REASON_TX_ACTIVITY:
		return "tx-activity";
	case O3_RX_REASON_TX_BURST_CHANGED:
		return "tx-burst-changed";
	case O3_RX_REASON_TX_BURST_PLOAMU_UNDERRUN:
		return "tx-burst-plus-ploamu-underrun";
	case O3_RX_REASON_TX_GEM_CHANGED:
		return "tx-gem-changed";
	case O3_RX_REASON_ONU_ID_CHANGED:
		return "onu-id-changed";
	case O3_RX_REASON_GLOBAL_CONFIG_CHANGED:
		return "global-config-changed";
	case O3_RX_REASON_PLOAMU_CHANGED:
		return "ploamu-changed";
	case O3_RX_REASON_XPON_GUARD:
		return "xpon-guard";
	case O3_RX_REASON_POLL_GAP:
		return "poll-gap-exceeded";
	case O3_RX_REASON_MUX_CHANGED:
		return "wan-conf-changed";
	case O3_RX_REASON_MUX_SELECT_WRITE:
		return "mux-select-write-error";
	case O3_RX_REASON_MUX_SELECT_READBACK:
		return "mux-select-readback-error";
	case O3_RX_REASON_PRE_OBSERVATION_GUARD:
		return "pre-observation-guard";
	case O3_RX_REASON_PRE_OBSERVATION_PRECONDITION:
		return "pre-observation-precondition";
	case O3_RX_REASON_ACTIVATION_READBACK:
		return "activation-readback-error";
	case O3_RX_REASON_DOWNSTREAM_PROGRESS:
		return "downstream-progress";
	case O3_RX_REASON_TRIGGER_NOT_EXACT:
		return "trigger-not-exact";
	case O3_RX_REASON_FINAL_PRE_POP_CHANGED:
		return "final-pre-pop-changed";
	case O3_RX_REASON_FIFO_CHANGED_AFTER_O1:
		return "fifo-changed-after-o1";
	case O3_RX_REASON_PRE_POP_MUX_CHANGED:
		return "pre-pop-mux-changed";
	case O3_RX_REASON_PRE_POP_GUARD:
		return "pre-pop-guard";
	case O3_RX_REASON_FIFO_POP_SLOW:
		return "fifo-pop-slow";
	case O3_RX_REASON_FIFO_POST_STATUS:
		return "fifo-post-status";
	case O3_RX_REASON_POST_POP_MUX_CHANGED:
		return "post-pop-mux-changed";
	case O3_RX_REASON_POST_POP_UNSAFE:
		return "post-pop-unsafe";
	case O3_RX_REASON_FIRST_POP_BOUNDARY:
		return "first-pop-boundary";
	case O3_RX_REASON_TWO_POP_COMPLETE:
		return "two-pop-complete";
	case O3_RX_REASON_INITIAL_RECORDS_INVALID:
		return "initial-records-invalid";
	case O3_RX_REASON_O3_O4_PLOAMU_HW_AUTO:
		return "o3-o4-ploamu-hw-auto";
	case O3_RX_REASON_PLOAMU_CTRL_FORCE:
		return "ploamu-control-force";
	case O3_RX_REASON_PLOAMU_CTRL_INVARIANT:
		return "ploamu-control-invariant";
	case O3_RX_REASON_PLOAMU_CTRL_RESTORE:
		return "ploamu-control-restore";
	case O3_RX_REASON_FORMATTER_WRITE:
		return "formatter-write";
	case O3_RX_REASON_O3_ACTIVATION:
		return "o3-activation";
	case O3_RX_REASON_O3_GUARD:
		return "o3-guard";
	case O3_RX_REASON_O3_FIFO_PARTIAL:
		return "o3-fifo-partial";
	case O3_RX_REASON_O3_RECORD_LIMIT:
		return "o3-record-limit";
	case O3_RX_REASON_O3_POLL_LIMIT:
		return "o3-poll-limit";
	case O3_RX_REASON_O3_POLL_GAP:
		return "o3-poll-gap";
	case O3_RX_REASON_O3_FIFO_OVERRUN:
		return "o3-fifo-overrun";
	case O3_RX_REASON_O3_EXT_BURST_WRITE:
		return "o3-extended-burst-write";
	case O3_RX_REASON_O3_OBSERVATION_COMPLETE:
		return "o3-observation-complete";
	case O3_RX_REASON_CLEANUP_BOUNDARY_CHANGED:
		return "cleanup-boundary-changed";
	case O3_RX_REASON_FORMATTER_RESTORE:
		return "formatter-restore";
	}

	return "unknown";
}

static const char *
cleanup_class_name(enum o3_rx_cleanup_class class)
{
	switch (class) {
	case O3_RX_CLEANUP_NONE:
		return "none";
	case O3_RX_CLEANUP_TX_BURST_PLUS_ONE:
		return "tx-burst-plus-one";
	case O3_RX_CLEANUP_TX_BURST_PLUS_ONE_PLOAMU_UNDERRUN:
		return "tx-burst-plus-one-ploamu-bit31-only";
	}

	return "unknown";
}

static const char *status_name(void)
{
	if (result.restore_unsafe && result.sn_msg_cfg_rdm_dly_residual &&
	    result.cold_power_cycle_required &&
	    result.stop_reason == O3_RX_REASON_O3_OBSERVATION_COMPLETE &&
	    !result.sequence_result)
		return "o3-observed-powercycle-required";
	if (result.restore_unsafe)
		return "unsafe-pinned";
	if (result.stop_reason == O3_RX_REASON_O3_O4_PLOAMU_HW_AUTO &&
	    result.sequence_result == -EPERM && !result.activation_write_attempted &&
	    !result.formatter_program_attempted && !result.gpon_writes &&
	    !result.xpon_formatter_writes && !result.restore_unsafe)
		return "o3-hw-auto-ploam-rejected-restored";
	if (!result.update_attempted && result.sequence_result)
		return "preflight-rejected-no-touch";
	if (result.stop_reason == O3_RX_REASON_O3_OBSERVATION_COMPLETE &&
	    result.records_valid && !result.sequence_result)
		return "o3-rx-observation-complete-restored";
	if (result.stop_reason == O3_RX_REASON_TWO_POP_COMPLETE &&
	    result.records_valid)
		return "two-pop-complete-restored";
	if (result.stop_reason == O3_RX_REASON_DOWNSTREAM_PROGRESS)
		return "downstream-progress-restored";
	if (result.sequence_result)
		return "aborted-restored";
	if (result.stop_reason == O3_RX_REASON_TIMELINE_COMPLETE)
		return "timeline-complete-restored";

	return "restored";
}

static const char *downstream_trigger_name(u32 trigger)
{
	switch (trigger) {
	case 0:
		return "none";
	case O3_RX_DS_FIFO_LEVEL:
		return "fifo-level";
	case O3_RX_DS_PLOAMD_STATUS:
		return "int-status-bit0";
	case O3_RX_DS_SN_REQ:
		return "sn-request-bit2";
	case O3_RX_DS_RANGING_REQ:
		return "ranging-request-bit4";
	case O3_RX_DS_SN_REQ_CRS:
		return "sn-request-crs-bit6";
	case O3_RX_DS_FIFO_LEVEL | O3_RX_DS_PLOAMD_STATUS:
		return "fifo-level+int-status-bit0";
	}

	return "combined";
}

static void print_guard(struct seq_file *s, const char *name,
			const struct xpon_guard_snapshot *g)
{
	seq_printf(s,
		   "%s: tx_disable_raw=%d physet2=%08x fwrdy=%u status_bit=%u physet3=%08x continuous_mode=%u physet10=%08x physta1=%08x fsm=%u setting=%08x misc=%08x rx=%08x sync=%x fec=%u sta=%08x los=%u prbs=%08x test=%08x int_en=%08x int_sts=%08x\n",
		   name, g->tx_disable_raw, g->physet2,
		   !!(g->physet2 & PHYSET2_FW_READY),
		   !!(g->physet2 & PHYSET2_STATUS_BIT), g->physet3,
		   !!(g->physet3 & PHYSET3_CONTINUOUS_MODE), g->physet10,
		   g->physta1,
		   (u32)FIELD_GET(PHYSTA1_STATE, g->physta1), g->setting,
		   g->misc, g->phyrx_status,
		   (u32)FIELD_GET(PHYRX_SYNC, g->phyrx_status),
		   !!(g->phyrx_status & PHYRX_FEC), g->trans_status,
		   !!(g->trans_status & TRANS_STATUS_LOS), g->prbs_tx,
		   g->test_frame, g->int_enable, g->int_status);
}

static void print_tx_correlation(
	struct seq_file *s, const char *name,
	const struct o3_rx_tx_correlation_snapshot *snapshot)
{
	seq_printf(s,
		   "%s_tx_correlation: mac_tx_gem=%u phy_tx_status_raw_unlatched=%08x phy_tx_frame_raw_unlatched=%08x phy_tx_burst_raw_unlatched=%08x phy_raw_policy=diagnostic_non_decisional unchanged_non_conclusive=1\n",
		   name, snapshot->mac_tx_gem_count,
		   snapshot->phy_tx_status_raw_unlatched,
		   snapshot->phy_tx_frame_count_raw_unlatched,
		   snapshot->phy_tx_burst_count_raw_unlatched);
}

static void print_mac(struct seq_file *s, const char *name,
		      const struct gpon_mac_snapshot *m,
		      u32 previous_interrupt_status)
{
	u32 new_events = m->interrupt_status & ~previous_interrupt_status;

	seq_printf(s, "%s: elapsed_ns=%llu onu_id_valid=%u global=%08x int_status=%08x int_new=%08x int_enable=%08x ploamu=%08x avail=%u min=%u underrun=%u ploamd=%08x used=%u max=%u overrun=%u activation=%08x state=O%u rx_gtc=%u tx_burst=%u\n",
		   name, m->elapsed_ns, !!(m->onu_id & ONU_ID_VALID),
		   m->global_config, m->interrupt_status, new_events,
		   m->interrupt_enable,
		   m->ploamu_fifo_status,
		   (u32)FIELD_GET(PLOAM_FIFO_LEVEL, m->ploamu_fifo_status),
		   (u32)FIELD_GET(PLOAM_FIFO_MAX_USED,
				  m->ploamu_fifo_status),
		   !!(m->ploamu_fifo_status & PLOAMU_FIFO_UNDERRUN),
		   m->ploamd_fifo_status,
		   (u32)FIELD_GET(PLOAM_FIFO_LEVEL, m->ploamd_fifo_status),
		   (u32)FIELD_GET(PLOAM_FIFO_MAX_USED,
				  m->ploamd_fifo_status),
		   !!(m->ploamd_fifo_status & PLOAMD_FIFO_OVERRUN),
		   m->activation_status,
		   (u32)FIELD_GET(ACTIVATION_STATE, m->activation_status),
		   m->rx_gtc_count, m->tx_burst_count);
	seq_printf(s,
		   "%s_events: ploamd_recv=%u ploamu_send=%u sn_req_recv=%u sn_send_o3=%u ranging_req=%u sn_send_o4=%u sn_req_crs=%u dying_gasp_send=%u\n",
		   name, !!(m->interrupt_status & INT_PLOAMD_RECV),
		   !!(m->interrupt_status & INT_PLOAMU_SEND),
		   !!(m->interrupt_status & INT_SN_REQ_RECV),
		   !!(m->interrupt_status & INT_SN_ONU_SEND_O3),
		   !!(m->interrupt_status & INT_RANGING_REQ_RECV),
		   !!(m->interrupt_status & INT_SN_ONU_SEND_O4),
		   !!(m->interrupt_status & INT_SN_REQ_CRS),
		   !!(m->interrupt_status & INT_DYING_GASP_SEND));
	seq_printf(s,
		   "%s_errors: rx=%u fifo=%u burst_signal=%u late_start=%u rx_eof=%u rx_gem_interleave=%u bfifo_full=%u sfifo_full=%u o5_eqd_done=%u olt_ds_fec_change=%u onu_us_fec_change=%u popup_o6=%u fwi=%u lwi=%u stop_time=%u bwm_us_fec=%u\n",
		   name, !!(m->interrupt_status & INT_RX_ERR),
		   !!(m->interrupt_status & INT_FIFO_ERR),
		   !!(m->interrupt_status & INT_BST_SGL_DIFF),
		   !!(m->interrupt_status & INT_TX_LATE_START),
		   !!(m->interrupt_status & INT_RX_EOF_ERR),
		   !!(m->interrupt_status & INT_RX_GEM_INTLV_ERR),
		   !!(m->interrupt_status & INT_BFIFO_FULL),
		   !!(m->interrupt_status & INT_SFIFO_FULL),
		   !!(m->interrupt_status & INT_O5_EQD_ADJ_DONE),
		   !!(m->interrupt_status & INT_OLT_DS_FEC_CHG),
		   !!(m->interrupt_status & INT_ONU_US_FEC_CHG),
		   !!(m->interrupt_status & INT_POPUP_RECV_O6),
		   !!(m->interrupt_status & INT_FWI),
		   !!(m->interrupt_status & INT_LWI),
		   !!(m->interrupt_status & INT_BWM_STOP_TIME_ERR),
		   !!(m->interrupt_status & INT_BWM_US_FEC_ERR));
	if (m->tx_gem_valid)
		seq_printf(s, "%s_mac_tx_gem: %u\n", name,
			   m->tx_correlation.mac_tx_gem_count);
	if (m->tx_correlation_valid)
		print_tx_correlation(s, name, &m->tx_correlation);
}

static void print_counters(struct seq_file *s, const char *name,
			   const struct gpon_mac_snapshot *m)
{
	seq_printf(s,
		   "%s_counters: rx_gem=%u rx_crc_error=%u rx_gtc=%u tx_burst=%u hec_one_error=%u hec_two_error=%u hec_uncorrectable=%u ds_spf=%u\n",
		   name, m->rx_gem_count, m->rx_crc_error_count,
		   m->rx_gtc_count, m->tx_burst_count,
		   m->rx_hec_one_error_count, m->rx_hec_two_error_count,
		   m->rx_hec_uncorrectable_count, m->ds_spf_count);
}

static void print_readonly_config(struct seq_file *s, const char *name,
				  const struct gpon_mac_snapshot *m)
{
	seq_printf(s,
		   "%s_readonly: rsp_time=%08x mbi_stop=%08x cap_setting=%08x dbg_dly=%08x idle_gem_thld=%08x ploamd_filter_o5=%08x o3_o4_ploamu_ctrl=%08x\n",
		   name, m->response_time, m->mbi_stop,
		   m->dbg_cap_setting, m->dbg_delay,
		   m->dbg_idle_gem_threshold,
		   m->dbg_ploamd_filter_in_o5,
		   m->o3_o4_ploamu_control);
}

static void print_formatter(struct seq_file *s, const char *name,
			    const struct o3_rx_formatter_snapshot *f)
{
	seq_printf(s,
		   "%s: mac_guard=%08x mac_preamble12=%08x mac_preamble3=%08x mac_pre_delay=%08x phy_preamble=%08x phy_delimiter_guard=%08x phy_ext_preamble=%08x\n",
		   name, f->mac_guard, f->mac_preamble12,
		   f->mac_preamble3, f->mac_pre_delay, f->phy_preamble,
		   f->phy_delimiter_guard, f->phy_ext_preamble);
}

static const char *worker_state_name(int state)
{
	switch (state) {
	case O3_RX_WORKER_NOT_STARTED:
		return "not-started";
	case O3_RX_WORKER_RUNNING:
		return "running-pinned";
	case O3_RX_WORKER_DONE:
		return "done";
	case O3_RX_WORKER_UNSAFE_PINNED:
		return "unsafe-pinned";
	}

	return "unknown";
}

static int status_show(struct seq_file *s, void *unused)
{
	/* Pair with o3_rx_publish_worker_state before reading result fields. */
	int worker_state = smp_load_acquire(&o3_rx_worker_state);

	seq_puts(s,
		 "operation: pinned-kthread sleepable RX-only GPON MAC O2-to-O3 lab\n");
	seq_puts(s, "sequence: phase28 guard -> exact ATM->GPON -> exact O1/reset/TX baseline plus read-only MAC/PHY correlation -> optional explicit bit0-only O3/O4 software-PLOAM force -> O2 -> first GTC -> passive sticky-status samples -> exact PLOAMD trigger -> one exact O1 -> full pre-pop guard -> record0 -> full 9-to-6 guard -> exact second boundary -> record1 -> full 6-to-3 guard -> validate Upstream Overhead -> save/program audited formatter -> local O3 with GPIO16 TX_DISABLE asserted -> bounded complete-record drain/classify plus read-only MAC/PHY correlation -> immediate upstream abort with optional exact MAC-TX-burst +1 cleanup class, alone or paired with exact PLOAMu bit31-only underrun -> risk-reducing formatter/O1 restore -> retain software PLOAM control after every upstream latch -> exact ATM\n");
	seq_puts(s, "physical_tx_guard: GPIO16 TX_DISABLE is the sole confirmed physical kill; PHYSET3 bit5 is only a continuous/burst-mode invariant\n");
	seq_puts(s, "phy_tx_raw_policy: double-read second-value raw_unlatched diagnostics only; changed and unchanged values are non-decisional; unchanged is non-conclusive; counter latch untouched\n");
	seq_puts(s, "cleanup_ploamu_policy: exact read-only bit31-only underrun class; status is never written or cleared; result remains failed, software-controlled, unsafe-pinned and powercycle-required\n");
	seq_printf(s, "worker_state: %s\n",
		   worker_state_name(worker_state));
	seq_puts(s, "stop_machine_used: no\n");
	seq_puts(s,
		 "worker_model: dedicated kthread; module self-pin; no CPU affinity\n");
	seq_printf(s, "module_self_pin_retained: %u\n",
		   worker_state == O3_RX_WORKER_RUNNING ||
		   worker_state == O3_RX_WORKER_UNSAFE_PINNED);
	if (worker_state == O3_RX_WORKER_NOT_STARTED ||
	    worker_state == O3_RX_WORKER_RUNNING)
		return 0;

	seq_printf(s, "status: %s\n", status_name());
	seq_printf(s, "reason: %s\n", stop_reason_name(result.stop_reason));
	seq_printf(s, "restore_unsafe: %u\n", result.restore_unsafe);
	seq_printf(s, "unsafe_pinned: %u\n", result.unsafe_pinned);
	seq_printf(s, "update_attempted: %u\n", result.update_attempted);
	seq_printf(s, "preflight_passed: %u\n", result.preflight_passed);
	seq_printf(s, "activation_write_attempted: %u\n",
		   result.activation_write_attempted);
	seq_printf(s, "stage_a_passive_sampled: %u\n",
		   result.stage_a_passive_sampled);
	seq_printf(s, "stage_b_passive_sampled: %u\n",
		   result.stage_b_passive_sampled);
	seq_printf(s, "first_gtc_valid: %u\n",
		   result.first_gtc_valid);
	seq_printf(s, "stage_a_readback_valid: %u\n",
		   result.stage_a_readback_valid);
	seq_printf(s, "stage_a_end_valid: %u\n",
		   result.stage_a_end_valid);
	seq_printf(s, "stage_b_readback_valid: %u\n",
		   result.stage_b_readback_valid);
	seq_printf(s, "first_eof_reassert_valid: %u\n",
		   result.first_eof_reassert_valid);
	seq_printf(s, "observation_end_valid: %u\n",
		   result.observation_end_valid);
	seq_printf(s, "downstream_trigger_valid: %u\n",
		   result.downstream_trigger_valid);
	seq_printf(s, "unsafe_status_valid: %u\n",
		   result.unsafe_status_valid);
	seq_printf(s, "mac_before_valid: %u\n", result.mac_before_valid);
	if (result.mac_before_valid)
		seq_printf(s,
			   "o3_o4_ploamu_ctrl_gate: raw=%08x software=%u hw_auto=%u\n",
			   result.mac_before.o3_o4_ploamu_control,
			   !!(result.mac_before.o3_o4_ploamu_control &
			      O3_O4_PLOAMU_CTRL_SW),
			   !(result.mac_before.o3_o4_ploamu_control &
			     O3_O4_PLOAMU_CTRL_SW));
	seq_printf(s, "force_software_ploamu_control_requested: %u\n",
		   result.ploamu_control_force_requested);
	seq_printf(s, "ploamu_control_baseline_hw_auto: %u\n",
		   result.ploamu_control_baseline_hw_auto);
	seq_printf(s, "ploamu_control_force_attempted: %u\n",
		   result.ploamu_control_force_attempted);
	seq_printf(s, "ploamu_control_forced_valid: %u\n",
		   result.ploamu_control_forced_valid);
	seq_printf(s,
		   "ploamu_control_words: before=%08x forced=%08x program_readback=%08x after=%08x\n",
		   result.ploamu_control_before,
		   result.ploamu_control_forced,
		   result.ploamu_control_program_readback,
		   result.ploamu_control_after);
	seq_printf(s, "ploamu_control_program_non_bit0_preserved: %u\n",
		   result.ploamu_control_force_attempted &&
		   !((result.ploamu_control_before ^
		      result.ploamu_control_program_readback) &
		     ~O3_O4_PLOAMU_CTRL_SW));
	seq_printf(s, "ploamu_control_program_result: %d\n",
		   result.ploamu_control_program_result);
	seq_printf(s, "ploamu_control_restore_attempted: %u\n",
		   result.ploamu_control_restore_attempted);
	seq_printf(s, "ploamu_control_restored_valid: %u\n",
		   result.ploamu_control_restored_valid);
	seq_printf(s, "ploamu_control_restore_skipped_no_o1: %u\n",
		   result.ploamu_control_restore_skipped_no_o1);
	seq_printf(s, "ploamu_control_restore_skipped_guard: %u\n",
		   result.ploamu_control_restore_skipped_guard);
	seq_printf(s,
		   "ploamu_control_restore_skipped_upstream_latch: %u\n",
		   result.ploamu_control_restore_skipped_upstream_latch);
	seq_printf(s, "ploamu_control_restore_skipped_formatter: %u\n",
		   result.ploamu_control_restore_skipped_formatter);
	seq_printf(s,
		   "ploamu_control_restore: pre_guard=%d result=%d post_guard=%d writes=%u\n",
		   result.ploamu_control_restore_pre_guard_result,
		   result.ploamu_control_restore_result,
		   result.ploamu_control_restore_post_guard_result,
		   result.ploamu_control_writes);
	seq_printf(s, "mac_after_valid: %u\n", result.mac_after_valid);
	seq_printf(s, "fifo_pop_armed: %u\n", result.fifo_pop_armed);
	seq_printf(s, "fifo_pop_attempted: %u\n",
		   result.fifo_pop_attempted);
	seq_printf(s, "two_pop_completed: %u\n",
		   result.fifo_pop_completed);
	seq_printf(s, "record0_captured: %u\n",
		   result.first_record_captured);
	seq_printf(s, "record1_captured: %u\n",
		   result.second_record_captured);
	seq_printf(s, "records_valid: %u\n", result.records_valid);
	seq_printf(s, "initial_records_result: %d\n",
		   result.initial_records_result);
	seq_printf(s, "initial_records_identical: %u\n",
		   result.initial_records_identical);
	seq_printf(s, "third_record_matches: %u\n",
		   result.third_record_matches);
	seq_printf(s, "formatter_program_result: %d\n",
		   result.formatter_program_result);
	seq_printf(s, "formatter_programmed_valid: %u\n",
		   result.formatter_programmed_valid);
	seq_printf(s, "formatter_restored_valid: %u\n",
		   result.formatter_restored_valid);
	seq_printf(s, "activation_o3_result: %d\n",
		   result.activation_o3_result);
	seq_printf(s, "activation_o3_guard_result: %d\n",
		   result.activation_o3_guard_result);
	seq_printf(s, "activation_o3_guard_mask: %08x\n",
		   result.activation_o3_guard_mask);
	seq_printf(s, "activation_o3_valid: %u\n",
		   result.activation_o3_valid);
	seq_printf(s, "o3_observation_result: %d\n",
		   result.o3_observation_result);
	seq_printf(s, "o3_observation_started: %u\n",
		   result.o3_observation_started);
	seq_printf(s, "formatter_restore_result: %d\n",
		   result.formatter_restore_result);
	seq_printf(s, "record_count: %u\n", result.record_count);
	seq_printf(s, "max_records_used: %u\n", result.max_records_used);
	seq_printf(s, "observe_ms_used: %u\n", result.observe_ms_used);
	seq_printf(s, "upstream_overhead_count: %u\n",
		   result.upstream_overhead_count);
	seq_printf(s, "extended_burst_count: %u\n",
		   result.extended_burst_count);
	seq_printf(s, "other_record_count: %u\n",
		   result.other_record_count);
	seq_printf(s, "sn_request_seen: %u\n", result.sn_request_seen);
	seq_printf(s, "sn_internal_send_seen: %u\n",
		   result.sn_internal_send_seen);
	seq_printf(s, "identity_snapshot_unchanged: %u\n",
		   result.identity_snapshot_unchanged);
	seq_printf(s, "identity_changed_mask: %08x\n",
		   result.identity_changed_mask);
	seq_printf(s, "sn_msg_cfg_changed_fields: %08x\n",
		   result.sn_msg_cfg_changed_fields);
	seq_printf(s, "sn_msg_cfg_rdm_dly_only_allowed: %u\n",
		   result.sn_msg_cfg_rdm_dly_only_allowed);
	seq_printf(s, "sn_msg_cfg_rdm_dly_residual: %u\n",
		   result.sn_msg_cfg_rdm_dly_residual);
	seq_printf(s, "cold_power_cycle_required: %u\n",
		   result.cold_power_cycle_required);
	seq_printf(s, "onu_id_never_valid: %u\n",
		   result.onu_id_never_valid);
	seq_printf(s, "extended_burst_seen: %u\n",
		   result.extended_burst_seen);
	seq_printf(s, "extended_burst_formatter_writes: %u\n",
		   result.extended_burst_formatter_writes);
	seq_printf(s, "o3_tx_burst_changed: %u last=%u\n",
		   result.o3_tx_burst_changed, result.o3_tx_burst_last);
	seq_printf(s, "o3_tx_correlation_valid: %u\n",
		   result.o3_tx_correlation_valid);
	if (result.o3_tx_correlation_valid)
		print_tx_correlation(s, "o3_last",
				     &result.o3_tx_correlation_last);
	seq_printf(s,
		   "phy_tx_raw_changed_seen: status=%u frame=%u burst=%u (diagnostic_non_decisional=1)\n",
		   result.phy_tx_status_raw_changed_seen,
		   result.phy_tx_frame_raw_changed_seen,
		   result.phy_tx_burst_raw_changed_seen);
	seq_printf(s,
		   "cleanup_tx_burst_latch: attempts=%u latched=%u value=%u reject_mask=%08x\n",
		   result.cleanup_tx_burst_latch_attempts,
		   result.cleanup_tx_burst_latched,
		   result.cleanup_tx_burst_latched_value,
		   result.cleanup_tx_burst_latch_reject_mask);
	seq_printf(s,
		   "cleanup_tx_burst_guard: checks=%u failures=%u last=%u changed_again=%u\n",
		   result.cleanup_tx_burst_guard_checks,
		   result.cleanup_tx_burst_guard_failures,
		   result.cleanup_tx_burst_guard_last,
		   result.cleanup_tx_burst_changed_again);
	seq_printf(s, "cleanup_class: %s eligible_now=%u sources=%08x\n",
		   cleanup_class_name(result.cleanup_class),
		   o3_rx_cleanup_tx_burst_eligible(),
		   o3_rx_cleanup_class_sources(result.cleanup_class));
	seq_printf(s,
		   "cleanup_ploamu_accept: status=%08x delta=%08x baseline_underrun=%u\n",
		   result.cleanup_ploamu_accepted_status,
		   result.cleanup_ploamu_accepted_delta,
		   !!(result.mac_before.ploamu_fifo_status &
		      PLOAMU_FIFO_UNDERRUN));
	seq_printf(s,
		   "cleanup_ploamu_guard: last=%08x changed_again=%u never_written_or_cleared=1\n",
		   result.cleanup_ploamu_guard_last,
		   result.cleanup_ploamu_changed_again);
	seq_printf(s,
		   "cleanup_ploamd_guard: accepted=%08x last=%08x changed_again=%u\n",
		   result.cleanup_ploamd_accepted_status,
		   result.cleanup_ploamd_guard_last,
		   result.cleanup_ploamd_changed_again);
	seq_printf(s,
		   "cleanup_irq_guard: unsafe_last=%08x unsafe_mask=%08x changed_again=%u allowed_mask=%08x\n",
		   result.cleanup_irq_unsafe_last,
		   result.cleanup_irq_unsafe_mask,
		   result.cleanup_irq_changed_again,
		   (u32)O3_RX_O3_ALLOWED_STATUS);
	seq_printf(s, "upstream_activity_latch: latched=%u sources=%08x\n",
		   result.upstream_activity_latched,
		   result.upstream_activity_latch_sources);
	seq_printf(s,
		   "upstream_activity_sources: irq=%u mac_tx_burst=%u ploamu=%u mac_tx_gem=%u\n",
		   !!(result.upstream_activity_latch_sources &
		      O3_RX_UPSTREAM_LATCH_TX_STATUS),
		   !!(result.upstream_activity_latch_sources &
		      O3_RX_UPSTREAM_LATCH_TX_BURST),
		   !!(result.upstream_activity_latch_sources &
		      O3_RX_UPSTREAM_LATCH_PLOAMU),
		   !!(result.upstream_activity_latch_sources &
		      O3_RX_UPSTREAM_LATCH_TX_GEM));
	seq_printf(s, "cleanup_tx_correlation_valid: %u samples=%u\n",
		   result.cleanup_tx_correlation_valid,
		   result.cleanup_tx_correlation_samples);
	if (result.cleanup_tx_correlation_valid)
		print_tx_correlation(s, "cleanup_last",
				     &result.cleanup_tx_correlation_last);
	seq_printf(s, "o3_ploamu_status_changed: %u last=%08x\n",
		   result.o3_ploamu_status_changed,
		   result.o3_ploamu_status_last);
	seq_printf(s, "private_records_available: %u\n",
		   result.first_record_captured);
	seq_printf(s, "private_records_verified: %u\n",
		   result.records_valid && !result.restore_unsafe &&
		   !result.sequence_result);
	seq_printf(s, "fifo_after_first_valid: %u\n",
		   result.fifo_after_first_valid);
	seq_printf(s, "guard_after_first_valid: %u\n",
		   result.guard_after_first_valid);
	seq_printf(s, "second_pre_pop_valid: %u\n",
		   result.second_pre_pop_valid);
	seq_printf(s, "guard_after_valid: %u\n",
		   result.guard_after_valid);
	seq_printf(s, "first_gtc_polls: %u\n",
		   result.first_gtc_polls);
	seq_printf(s, "passive_trigger_polls: %u\n",
		   result.passive_trigger_polls);
	seq_printf(s, "o3_poll_count: %u max_gap_ns=%llu\n",
		   result.o3_poll_count, result.o3_max_poll_gap_ns);
	seq_printf(s, "stage_a_polls: %u\n", result.stage_a_polls);
	seq_printf(s, "stage_b_polls: %u\n", result.stage_b_polls);
	seq_printf(s, "checks: %u\n", result.checks);
	seq_printf(s, "first_poll_us: %u..%u\n",
		   GPON_FIRST_POLL_MIN_US, GPON_FIRST_POLL_MAX_US);
	seq_printf(s, "long_poll_us: %u..%u\n",
		   GPON_POLL_MIN_US, GPON_POLL_MAX_US);
	seq_printf(s, "poll_hard_gap_us: %u\n",
		   GPON_POLL_HARD_GAP_US);
	seq_printf(s, "max_check_gap_ns: %llu\n", result.max_check_gap_ns);
	seq_printf(s, "terminal_gap_ns: %llu\n", result.terminal_gap_ns);
	seq_printf(s, "gpon_mac_mmio_reads: %u\n", result.gpon_reads);
	seq_printf(s,
		   "mac_tx_gem_samples: %u full_tx_correlation_samples: %u phy_raw_unlatched_mmio_reads: %u\n",
		   result.mac_tx_gem_samples, result.tx_correlation_samples,
		   result.phy_tx_correlation_reads);
	seq_printf(s, "gpon_mac_mmio_writes: %u (O2/O3 activation, dedicated bit0-only PLOAM-control RMW, audited formatter, exact restore)\n",
		   result.gpon_writes);
	seq_printf(s, "xpon_formatter_writes: %u\n",
		   result.xpon_formatter_writes);
	seq_printf(s, "restore_write_attempts: %u\n",
		   result.restore_write_attempts);
	seq_printf(s, "restore_guard_checks: %u failures=%u\n",
		   result.restore_guard_checks, result.restore_guard_failures);
	seq_printf(s, "restore_readback_failures: %u\n",
		   result.restore_readback_failures);
	seq_printf(s,
		   "baseline_status=%08x expected=%08x|%08x|%08x expected_match=%u eof_preexisting=%u rx_gtc=%u unsafe=%08x\n",
		   result.baseline_status,
		   (u32)O3_RX_BASELINE_PRE_DS_FEC,
		   (u32)O3_RX_BASELINE_NO_EOF,
		   (u32)O3_RX_BASELINE_EOF_LATCHED,
		   result.baseline_expected_match,
		   result.baseline_eof_preexisting,
		   result.baseline_rx_gtc,
		   result.baseline_unsafe_mask);
	seq_printf(s,
		   "startup_status=%08x safe_mask=%08x expected=%08x unsafe=%08x expected_match=%u\n",
		   result.startup_status,
		   (u32)O3_RX_SAFE_STARTUP_MASK,
		   (u32)O3_RX_EXPECTED_STARTUP_MASK,
		   result.startup_unsafe_mask,
		   result.startup_expected_match);
	seq_printf(s,
		   "stage_a_passive_pre=%08x expected=%08x post=%08x non_target_lost=%08x sample_ns=%llu\n",
		   result.stage_a_pre_status,
		   (u32)O3_RX_PASSIVE_IDLE_STATUS,
		   result.stage_a_post_status,
		   result.stage_a_non_target_lost,
		   result.stage_a_readback_latency_ns);
	seq_printf(s,
		   "stage_b_passive_pre=%08x expected=%08x post=%08x non_target_lost=%08x sample_ns=%llu expected_match=%u\n",
		   result.stage_b_pre_status,
		   (u32)O3_RX_PASSIVE_IDLE_STATUS,
		   result.stage_b_post_status,
		   result.stage_b_non_target_lost,
		   result.stage_b_readback_latency_ns,
		   result.stage_b_expected_match);
	seq_printf(s, "downstream_trigger: %s (%08x)\n",
		   downstream_trigger_name(result.downstream_trigger_source),
		   result.downstream_trigger_source);
	seq_printf(s, "accepted_trigger_status=%08x expected=%08x\n",
		   result.accepted_trigger_status,
		   (u32)O3_RX_PASSIVE_TRIGGER_STATUS);
	seq_printf(s,
		   "final_pre_pop_valid=%u fifo=%08x status=%08x\n",
		   result.final_pre_pop_valid,
		   result.final_pre_pop_fifo_status,
		   result.final_pre_pop_status);
	seq_printf(s,
		   "second_pre_pop_valid=%u fifo=%08x status=%08x\n",
		   result.second_pre_pop_valid,
		   result.second_pre_pop_fifo_status,
		   result.second_pre_pop_status);
	seq_printf(s, "unsafe_interrupt_mask: %08x\n",
		   result.unsafe_interrupt_mask);
	seq_printf(s, "scu_update_calls: %u (WAN_MODE only)\n",
		   result.scu_update_calls);
	seq_puts(s, "irq_registered_or_enabled: no\n");
	seq_printf(s, "irq_enable_writes: %u\n", result.irq_enable_writes);
	seq_printf(s, "irq_status_writes: %u\n", result.irq_status_writes);
	seq_printf(s, "fifo_data_reads: %u (maximum %u; initial O2 pair is %u words; raw words withheld from status)\n",
		   result.fifo_data_reads,
		   O3_RX_MAX_RECORDS * O3_RX_PLOAMD_WORDS_PER_RECORD,
		   O3_RX_PLOAMD_TOTAL_WORDS);
	seq_printf(s, "identity_writes: %u\n", result.identity_writes);
	seq_printf(s, "upstream_fifo_writes: %u\n",
		   result.upstream_fifo_writes);
	seq_printf(s, "gpio_pinctrl_writes: %u\n",
		   result.gpio_pinctrl_writes);
	seq_printf(s, "non_formatter_xpon_writes: %u\n",
		   result.non_formatter_xpon_writes);
	seq_printf(s, "phy_laser_apd_en7570_writes: %u\n",
		   result.phy_laser_apd_en7570_writes);
	seq_printf(s, "first_gtc_timeout_us: %u\n",
		   GPON_FIRST_GTC_TIMEOUT_US);
	seq_printf(s, "passive_startup_settle_us: %u\n",
		   GPON_PASSIVE_STARTUP_SETTLE_US);
	seq_printf(s, "total_observe_us: %u\n",
		   GPON_TOTAL_OBSERVE_US);
	seq_printf(s, "o2_hard_limit_us: %u\n", GPON_O2_HARD_LIMIT_US);
	seq_printf(s, "sequence_result: %d\n", result.sequence_result);
	seq_printf(s, "o2_limit_result: %d\n", result.o2_limit_result);
	seq_printf(s, "activation_restore_result: %d\n",
		   result.activation_restore_result);
	seq_printf(s, "mac_after_result: %d\n", result.mac_after_result);
	seq_printf(s, "terminal_gap_result: %d\n",
		   result.terminal_gap_result);
	seq_printf(s, "restore_result: %d\n", result.restore_result);
	seq_printf(s, "final_read_result: %d\n", result.final_read_result);
	seq_printf(s, "guard_after_result: %d\n",
		   result.guard_after_result);
	seq_printf(s, "o1_restore_pre_guard_valid=%u result=%d\n",
		   result.o1_restore_pre_guard_valid,
		   result.o1_restore_pre_guard_result);
	seq_printf(s, "o1_restore_post_guard_valid=%u result=%d\n",
		   result.o1_restore_post_guard_valid,
		   result.o1_restore_post_guard_result);
	seq_printf(s, "restore_mode_guard_valid=%u result=%d\n",
		   result.restore_mode_guard_valid,
		   result.restore_mode_guard_result);
	seq_printf(s, "trigger_gate_result: %d\n",
		   result.trigger_gate_result);
	seq_printf(s, "pre_pop_result: %d\n", result.pre_pop_result);
	seq_printf(s, "guard_pre_pop_result: %d\n",
		   result.guard_pre_pop_result);
	seq_printf(s, "fifo_pop_limit_result: %d\n",
		   result.fifo_pop_limit_result);
	seq_printf(s, "first_pop_post_result: %d\n",
		   result.first_pop_post_result);
	seq_printf(s, "guard_after_first_result: %d\n",
		   result.guard_after_first_result);
	seq_printf(s, "second_pre_pop_result: %d\n",
		   result.second_pre_pop_result);
	seq_printf(s, "fifo_post_result: %d\n",
		   result.fifo_post_result);
	seq_printf(s, "guard_post_pop_result: %d\n",
		   result.guard_post_pop_result);
	seq_printf(s, "post_pop_mux_result: %d\n",
		   result.post_pop_mux_result);
	seq_printf(s, "wan_before=%08x wan_gpon=%08x wan_after=%08x wan_mismatch=%08x reset2=%08x reset1=%08x\n",
		   result.wan_before, result.wan_gpon, result.wan_after,
		   result.wan_mismatch, result.reset_ctrl2,
		   result.reset_ctrl1);
	seq_printf(s, "wan_post_pop_valid=%u wan_post_pop=%08x\n",
		   result.wan_post_pop_valid, result.wan_post_pop);
	seq_printf(s, "hold_ns=%llu cycle_ns=%llu\n",
		   result.hold_ns, result.cycle_ns);
	seq_printf(s, "fifo_pop_ns=%llu first_pop_ns=%llu second_pop_ns=%llu trigger_to_o1_ns=%llu trigger_to_post_ns=%llu irq_off_ns=%llu\n",
		   result.fifo_pop_ns, result.first_pop_ns, result.second_pop_ns,
		   result.trigger_to_o1_ns, result.trigger_to_post_ns,
		   result.irq_off_ns);
	seq_printf(s,
		   "activation_before=%08x activation_o2=%08x activation_after=%08x\n",
		   result.activation_before, result.activation_o2,
		   result.activation_after);
	seq_printf(s,
		   "first_gtc_elapsed_ns=%llu stage_a_clear_elapsed_ns=%llu stage_a_end_elapsed_ns=%llu stage_b_clear_elapsed_ns=%llu\n",
		   result.first_gtc_elapsed_ns,
		   result.stage_a_clear_elapsed_ns,
		   result.stage_a_end_elapsed_ns,
		   result.stage_b_clear_elapsed_ns);
	seq_printf(s, "first_eof_reassert_latency_ns=%llu\n",
		   result.first_eof_reassert_latency_ns);
	print_guard(s, "guard_before", &result.guard_before);
	if (result.mac_before_valid) {
		print_mac(s, "mac_before", &result.mac_before, 0);
		print_counters(s, "mac_before", &result.mac_before);
		print_readonly_config(s, "mac_before",
				      &result.mac_before);
	}
	if (result.first_gtc_valid)
		print_mac(s, "first_gtc", &result.first_gtc,
			  result.mac_before.interrupt_status);
	if (result.stage_a_readback_valid)
		print_mac(s, "stage_a_readback",
			  &result.stage_a_readback,
			  result.stage_a_pre_status);
	if (result.first_eof_reassert_valid)
		print_mac(s, "first_eof_reassert",
			  &result.first_eof_reassert,
			  result.stage_a_post_status);
	if (result.stage_a_end_valid) {
		print_mac(s, "stage_a_end", &result.stage_a_end,
			  result.stage_a_post_status);
		print_counters(s, "stage_a_end",
			       &result.stage_a_end);
		print_readonly_config(s, "stage_a_end",
				      &result.stage_a_end);
	}
	if (result.stage_b_readback_valid)
		print_mac(s, "stage_b_readback",
			  &result.stage_b_readback,
			  result.stage_b_pre_status);
	if (result.observation_end_valid) {
		print_mac(s, "observation_end", &result.observation_end,
			  result.stage_b_post_status);
		print_counters(s, "observation_end",
			       &result.observation_end);
		print_readonly_config(s, "observation_end",
				      &result.observation_end);
	}
	if (result.downstream_trigger_valid)
		print_mac(s, "downstream_trigger",
			  &result.downstream_trigger,
			  result.mac_before.interrupt_status);
	if (result.unsafe_status_valid)
		print_mac(s, "unsafe_status", &result.unsafe_status,
			  result.mac_before.interrupt_status);
	if (result.mac_after_valid) {
		print_mac(s, "mac_after", &result.mac_after,
			  result.mac_before.interrupt_status);
		print_counters(s, "mac_after", &result.mac_after);
		print_readonly_config(s, "mac_after",
				      &result.mac_after);
		seq_printf(s, "mac_after_result: %d\n",
			   result.mac_after_result);
	}
	if (result.guard_pre_pop_valid)
		print_guard(s, "guard_pre_pop", &result.guard_pre_pop);
	if (result.fifo_after_first_valid)
		print_mac(s, "fifo_after_first", &result.fifo_after_first,
			  result.mac_after.interrupt_status);
	if (result.guard_after_first_valid)
		print_guard(s, "guard_after_first", &result.guard_after_first);
	if (result.fifo_post_valid)
		print_mac(s, "fifo_after_second", &result.fifo_post,
			  result.mac_after.interrupt_status);
	if (result.guard_post_pop_valid)
		print_guard(s, "guard_post_pop", &result.guard_post_pop);
	if (result.formatter_before_valid)
		print_formatter(s, "formatter_before",
				&result.formatter_before);
	if (result.formatter_programmed_valid)
		print_formatter(s, "formatter_programmed",
				&result.formatter_programmed);
	if (result.o3_final_valid) {
		print_mac(s, "o3_final", &result.o3_final,
			  result.mac_before.interrupt_status);
		print_counters(s, "o3_final", &result.o3_final);
		print_readonly_config(s, "o3_final", &result.o3_final);
	}
	if (result.o3_guard_final_valid)
		print_guard(s, "o3_guard_final", &result.o3_guard_final);
	if (result.formatter_program_attempted)
		print_formatter(s, "formatter_after",
				&result.formatter_after);
	if (result.guard_after_valid)
		print_guard(s, "guard_after", &result.guard_after);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

/*
 * Keep operator-originated PLOAM data out of the ordinary status report and
 * every kernel log.  This separate root-only debugfs file is intended to be
 * redirected directly into the private lab capture directory.  It becomes
 * readable only after the worker reaches a terminal state.  If a post-read or
 * restoration check failed, the private output preserves the captured words
 * but marks them unverified so a required power-cycle cannot destroy the only
 * copy before the host saves it.
 */
static int private_records_show(struct seq_file *s, void *unused)
{
	/* Pair with o3_rx_publish_worker_state before reading result fields. */
	int worker_state = smp_load_acquire(&o3_rx_worker_state);
	unsigned int i;

	if (worker_state == O3_RX_WORKER_NOT_STARTED ||
	    worker_state == O3_RX_WORKER_RUNNING)
		return -EAGAIN;
	if ((worker_state != O3_RX_WORKER_DONE &&
	     worker_state != O3_RX_WORKER_UNSAFE_PINNED) ||
	    !result.first_record_captured)
		return -ENODATA;

	seq_puts(s, "format: up to max_records_used three-word GPON_G_PLOAMD_RDATA records in hardware read order; initial two records are the bounded O2 pair; MIPS big-endian values; no host byte reinterpretation\n");
	seq_printf(s, "records_requested: %u\n", result.max_records_used);
	seq_printf(s, "records_captured: %u\n", result.record_count);
	seq_printf(s, "initial_records: %u\n", O3_RX_INITIAL_RECORDS);
	seq_printf(s, "records_valid: %u\n", result.records_valid);
	seq_printf(s, "record0_captured: %u\n", result.first_record_captured);
	seq_printf(s, "record1_captured: %u\n", result.second_record_captured);
	seq_printf(s, "restore_unsafe: %u\n", result.restore_unsafe);
	seq_printf(s, "sequence_result: %d\n", result.sequence_result);
	seq_printf(s, "fifo_before: %08x\n",
		   result.mac_after.ploamd_fifo_status);
	seq_printf(s, "fifo_after_record0: %08x\n",
		   result.fifo_after_first.ploamd_fifo_status);
	if (result.second_record_captured)
		seq_printf(s, "fifo_after_record1: %08x\n",
			   result.fifo_post.ploamd_fifo_status);
	for (i = 0; i < result.record_count && i < O3_RX_MAX_RECORDS; i++) {
		seq_printf(s, "record%u_class: %u\n", i,
			   result.record_class[i]);
		seq_printf(s, "record%u_word0: %08x\n", i,
			   result.ploamd_words[i][0]);
		seq_printf(s, "record%u_word1: %08x\n", i,
			   result.ploamd_words[i][1]);
		seq_printf(s, "record%u_word2: %08x\n", i,
			   result.ploamd_words[i][2]);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(private_records);

static int verify_phase28_binding(void)
{
	struct platform_device *pdev;
	struct device_node *node;
	int ret = 0;

	node = of_find_compatible_node(NULL, NULL,
				       "econet,en751221-en7570-rx-handoff-experimental");
	if (!node)
		return -ENODEV;
	if (!of_property_read_bool(node, "econet,allow-en7570-rx-handoff")) {
		ret = -EPERM;
		goto out_node;
	}

	pdev = of_find_device_by_node(node);
	if (!pdev) {
		ret = -EPROBE_DEFER;
		goto out_node;
	}
	if (!pdev->dev.driver ||
	    strcmp(pdev->dev.driver->name,
		   "xr500v-en7570-rx-handoff-observer"))
		ret = -ENOLINK;
	put_device(&pdev->dev);

out_node:
	of_node_put(node);
	return ret;
}

static void o3_rx_release_hw_resources(void)
{
	if (!o3_rx_hw_resources)
		return;

	iounmap(o3_rx_ctx.gpon);
	o3_rx_ctx.gpon = NULL;
	release_mem_region(GPON_MAC_PHYS, GPON_MAC_SIZE);
	iounmap(o3_rx_ctx.xpon);
	o3_rx_ctx.xpon = NULL;
	o3_rx_hw_resources = false;
}

static int o3_rx_worker(void *unused)
{
	int err;

	err = o3_rx_run(&o3_rx_ctx);
	result.sequence_result = err;

	if (result.restore_unsafe) {
		result.unsafe_pinned = true;
		o3_rx_publish_worker_state(O3_RX_WORKER_UNSAFE_PINNED);
		if (result.sn_msg_cfg_rdm_dly_residual && !err &&
		    result.stop_reason == O3_RX_REASON_O3_OBSERVATION_COMPLETE)
			pr_warn("o3_rx: O3 observation complete with residual SN-message random-delay state; module self-pin and physical power-cycle required\n");
		else
			pr_emerg("o3_rx: restoration verification failed; module self-pin and hardware resources retained; physically power-cycle before recovery\n");
		kthread_exit(err ? err : -EIO);
	}

	o3_rx_release_hw_resources();
	o3_rx_publish_worker_state(O3_RX_WORKER_DONE);
	if (err && !result.update_attempted)
		pr_warn("o3_rx: preflight rejected with %d before any hardware write\n",
			err);
	else if (err)
		pr_warn("o3_rx: O3 RX lab stopped with %d; exact O1/ATM and physical guards restored\n",
			err);
	else
		pr_warn("o3_rx: O3 RX observation complete; exact O1/ATM restored; GPIO16 TX_DISABLE retained\n");

	module_put_and_kthread_exit(0);
}

static int __init o3_rx_init(void)
{
	struct dentry *records_file;
	struct resource *region;
	struct task_struct *task;
	int err;

	if (!arm_o3_rx_lab)
		return -EPERM;
	if (!of_machine_is_compatible("tplink,archer-xr500v") ||
	    !of_machine_is_compatible("econet,en751221"))
		return -ENODEV;
	if (max_records < O3_RX_INITIAL_RECORDS ||
	    max_records > O3_RX_MAX_RECORDS ||
	    observe_ms < O3_RX_MIN_OBSERVE_MS ||
	    observe_ms > O3_RX_MAX_OBSERVE_MS)
		return -EINVAL;

	err = verify_phase28_binding();
	if (err)
		return err;

	o3_rx_ctx.tx_disable =
		gpio_to_desc(XR500V_TX_DISABLE_GPIO);
	if (!o3_rx_ctx.tx_disable ||
	    gpiod_cansleep(o3_rx_ctx.tx_disable) ||
	    gpiod_get_direction(o3_rx_ctx.tx_disable) != 0 ||
	    gpiod_get_raw_value(o3_rx_ctx.tx_disable) != 1)
		return -EPERM;

	o3_rx_ctx.scu =
		syscon_regmap_lookup_by_compatible("econet,en751221-scu");
	if (IS_ERR(o3_rx_ctx.scu))
		return PTR_ERR(o3_rx_ctx.scu);

	/* Shared read-only view of the xPON block owned by phase 28. */
	o3_rx_ctx.xpon =
		ioremap(XR500V_XPON_PHYS, XR500V_XPON_SIZE);
	if (!o3_rx_ctx.xpon)
		return -ENOMEM;

	region = request_mem_region(GPON_MAC_PHYS, GPON_MAC_SIZE,
				    KBUILD_MODNAME);
	if (!region) {
		err = -EBUSY;
		goto unmap_xpon;
	}
	o3_rx_ctx.gpon =
		ioremap(GPON_MAC_PHYS, GPON_MAC_SIZE);
	if (!o3_rx_ctx.gpon) {
		err = -ENOMEM;
		goto release_gpon;
	}
	o3_rx_hw_resources = true;

	memset(&result, 0, sizeof(result));
	result.max_records_used = max_records;
	result.observe_ms_used = observe_ms;
	result.ploamu_control_force_requested =
		force_software_ploamu_control;
	result.debugfs_dir =
		debugfs_create_dir("xr500v-gpon-o3-rx-lab", NULL);
	if (IS_ERR(result.debugfs_dir)) {
		err = PTR_ERR(result.debugfs_dir);
		result.debugfs_dir = NULL;
		goto remove_debugfs;
	}
	if (!result.debugfs_dir) {
		err = -ENOMEM;
		goto remove_debugfs;
	}
	if (IS_ERR_OR_NULL(debugfs_create_file("status", 0400,
					       result.debugfs_dir, NULL,
					       &status_fops))) {
		err = -ENOMEM;
		goto remove_debugfs;
	}
	records_file = debugfs_create_file("private_records", 0400,
					   result.debugfs_dir, NULL,
					   &private_records_fops);
	if (IS_ERR_OR_NULL(records_file)) {
		err = -ENOMEM;
		goto remove_debugfs;
	}

	o3_rx_publish_worker_state(O3_RX_WORKER_RUNNING);
	__module_get(THIS_MODULE);
	task = kthread_run(o3_rx_worker, NULL, "xr500v-gpon-o3rx");
	if (IS_ERR(task)) {
		err = PTR_ERR(task);
		module_put(THIS_MODULE);
		o3_rx_publish_worker_state(O3_RX_WORKER_NOT_STARTED);
		goto remove_debugfs;
	}

	pr_warn("o3_rx: pinned RX-only O3 worker started; status is available in debugfs; raw records will never be logged\n");
	return 0;

remove_debugfs:
	debugfs_remove_recursive(result.debugfs_dir);
	o3_rx_release_hw_resources();
	return err;

release_gpon:
	release_mem_region(GPON_MAC_PHYS, GPON_MAC_SIZE);
unmap_xpon:
	iounmap(o3_rx_ctx.xpon);
	o3_rx_ctx.xpon = NULL;
	return err;
}

static void __exit o3_rx_exit(void)
{
	if (result.restore_unsafe) {
		pr_emerg("o3_rx: refusing hardware resource cleanup after unsafe restoration; power-cycle required\n");
		return;
	}
	o3_rx_release_hw_resources();
	debugfs_remove_recursive(result.debugfs_dir);
}

module_init(o3_rx_init);
module_exit(o3_rx_exit);

MODULE_DESCRIPTION("XR500v pinned-kthread RX-only GPON MAC O2-to-O3 lab");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");

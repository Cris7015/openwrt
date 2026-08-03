include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-gpon-o3-rx-lab
PKG_RELEASE:=14

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-gpon-o3-rx-lab
  SUBMENU:=Other modules
  TITLE:=XR500v GPON MAC guarded O2-to-O3 RX-only lab
  DEPENDS:=@TARGET_econet
  FILES:=$(PKG_BUILD_DIR)/xr500v-gpon-o3-rx-lab.ko
endef

define KernelPackage/xr500v-gpon-o3-rx-lab/description
  Lab-only, non-shipping and non-autoload O3 RX experiment. It can run only
  after the phase-28 EN7570 RX handoff driver has reached stable PHY_READY.
  A dedicated sleepable kthread holds a module self-reference, keeps GPIO16
  TX_DISABLE asserted and PHYSET3 in its audited GPON burst-mode value,
  reproduces the proven O2 trigger
  and two downstream PLOAM pops, validates a broadcast Upstream Overhead pair,
  saves and programs only the audited MAC/xPON formatter words, enters a local
  O3 receive-only state, drains complete downstream records within hard bounds,
  classifies Extended Burst Length without applying its active formatter step,
  provides a default-off read-only opt-in which may force only
  O3_O4_PLOAMU_CTRL bit 0 from hardware-auto to software control,
  permits only an exact one-count MAC TX-burst latch, optionally paired with
  an exact bit-31-only PLOAMu underrun latch, for risk-reducing O1/formatter
  cleanup while retaining software control and requiring a power cycle,
  passively correlates the MAC TX-GEM counter with raw unlatched PHY
  TX status/frame/burst counters without touching the PHY counter latch,
  and attempts exact saved formatter restoration before returning to ATM. Raw
  PLOAM words are
  exposed only through a separate root-only debugfs file; ordinary status and
  kernel logs never print them. It keeps INT_ENABLE zero and never writes
  identity, upstream FIFO, IRQ enable, GPIO, EN7570, laser or APD state.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-gpon-o3-rx-lab))

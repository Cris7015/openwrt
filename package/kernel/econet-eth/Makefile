include $(TOPDIR)/rules.mk

PKG_NAME:=econet-eth
PKG_RELEASE:=2

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/cjdelisle/econet_eth.git
PKG_MIRROR_HASH:=dee7d98d0d9b55b443044d5ab792cd7ea94eb0d871d1929734c85dea2f942310
PKG_SOURCE_DATE:=2026-02-13
PKG_SOURCE_VERSION:=c2f855cf5bdcb2dfc536d865d21bc793a346ce44
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

define KernelPackage/econet-eth
  SUBMENU:=Network Devices
  TITLE:=EcoNet EN751221 Ethernet and cascaded DSA switch drivers
  DEPENDS:=@TARGET_econet +kmod-libphy +kmod-of-mdio +kmod-mdio-devres \
	+kmod-dsa
  # The in-tree MT7530/tagger exports symbols also provided by this EN751221
  # fork. Keep it disabled. PCS_MTK_LYNXI has no prompt in Linux 6.18, so its
  # source is built privately below instead of selecting the generic switch.
  KCONFIG:=CONFIG_MEDIATEK_GE_PHY=m \
	CONFIG_PCS_MTK_LYNXI=n \
	CONFIG_NET_DSA_TAG_MTK=n \
	CONFIG_NET_DSA_MT7530=n \
	CONFIG_NET_DSA_MT7530_MDIO=n \
	CONFIG_NET_DSA_MT7530_MMIO=n
  FILES:=$(PKG_BUILD_DIR)/econet-eth.ko \
	$(PKG_BUILD_DIR)/econet-gsw.ko \
	$(PKG_BUILD_DIR)/econet-gsw-mmio.ko \
	$(PKG_BUILD_DIR)/econet-gsw-mdio.ko \
	$(PKG_BUILD_DIR)/mtk-tag.ko \
	$(PKG_BUILD_DIR)/pcs-mtk-lynxi.ko \
	$(LINUX_DIR)/drivers/net/phy/mediatek/mtk-phy-lib.ko \
	$(LINUX_DIR)/drivers/net/phy/mediatek/mtk-ge.ko
endef

define KernelPackage/econet-eth/description
  Out-of-tree Frame Engine/QDMA driver plus the EN751221-specific nested
  MT7530 DSA switch core, MMIO/MDIO frontends and MediaTek tagger.
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) $(LINUX_DIR)/drivers/net/pcs/pcs-mtk-lynxi.c \
		$(PKG_BUILD_DIR)/pcs-mtk-lynxi-core.c
	$(CP) $(LINUX_DIR)/drivers/phy/phy-common-props.c $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

define KernelPackage/econet-eth/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/econet-switch.init $(1)/etc/init.d/econet-switch
endef

$(eval $(call KernelPackage,econet-eth))

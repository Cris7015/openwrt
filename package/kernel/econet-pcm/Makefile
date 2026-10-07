include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=econet-pcm
PKG_RELEASE:=9

include $(INCLUDE_DIR)/package.mk

define KernelPackage/econet-pcm
  SUBMENU:=Other modules
  TITLE:=EN751221 PCM/TDM + Le9642 SLIC (ZSI) drivers
  DEPENDS:=@TARGET_airoha_en751221
  FILES:=$(PKG_BUILD_DIR)/pcm-en751221.ko $(PKG_BUILD_DIR)/econet-slic.ko
  AUTOLOAD:=$(call AutoLoad,35,pcm-en751221)
endef

define KernelPackage/econet-pcm/description
  PCM/TDM controller driver for the EcoNet/TrendChip EN751221 SoC (0x1fbd0000)
  plus the Le9642 SLIC ZSI driver (0x1fbd1000). Built together so the SLIC can
  use the PCM's exported clock function. Foundation for VoIP/FXS support.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,econet-pcm))

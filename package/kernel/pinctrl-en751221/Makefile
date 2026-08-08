include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=pinctrl-en751221
PKG_RELEASE:=2

include $(INCLUDE_DIR)/package.mk

define KernelPackage/pinctrl-en751221
  SUBMENU:=Other modules
  TITLE:=EcoNet EN751221 pin control
  DEPENDS:=@TARGET_econet
  FILES:=$(PKG_BUILD_DIR)/pinctrl-en751221.ko
  AUTOLOAD:=$(call AutoLoad,20,pinctrl-en751221,1)
endef

define KernelPackage/pinctrl-en751221/description
  Pin multiplexing for the EcoNet EN751221 SoC through IOMUX_CONTROL1 in the
  chip SCU. Bit meanings were reconstructed from the OEM GPL sources.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,pinctrl-en751221))

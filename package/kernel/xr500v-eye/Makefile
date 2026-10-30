include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-eye
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-eye
  SUBMENU:=Other modules
  TITLE:=EN751221 xPON PHY eye monitor (lab)
  DEPENDS:=@TARGET_airoha_en751221
  FILES:=$(PKG_BUILD_DIR)/xr500v_eye.ko
endef

define KernelPackage/xr500v-eye/description
  Lab-only access to the eye sampler of the EN751221 xPON PHY (0x1faf0000)
  through debugfs xr500v-eye. It moves only the sampler phase and threshold,
  never the data path, and restores what it touched. Not autoloaded: the
  GPON panel loads it for its eye scan.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-eye))

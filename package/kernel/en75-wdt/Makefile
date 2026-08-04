include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=en75-wdt
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/en75-wdt
  SUBMENU:=Other modules
  TITLE:=EcoNet EN751221 watchdog for lockup capture
  DEPENDS:=@TARGET_econet
  FILES:=$(PKG_BUILD_DIR)/en75-wdt.ko
endef

define KernelPackage/en75-wdt/description
  Arms the SoC watchdog and kicks it from a kernel timer, so a lockup that
  stops timers from running resets the board instead of hanging it. The reset
  is warm, so the ramoops region survives and the last messages before the
  lockup can be read from /sys/fs/pstore afterwards.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,en75-wdt))

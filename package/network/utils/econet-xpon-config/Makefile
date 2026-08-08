include $(TOPDIR)/rules.mk

PKG_NAME:=econet-xpon-config
PKG_VERSION:=1
PKG_RELEASE:=2
PKG_MAINTAINER:=AK Sharma <maskachoska@yahoo.com>
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=COPYING

include $(INCLUDE_DIR)/package.mk

define Package/econet-xpon-config
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=WWAN
  TITLE:=EcoNet EN75xx GPON safe lab module loader
  DEPENDS:=@TARGET_econet +kmod-econet-xpon
  PKGARCH:=all
endef

define Package/econet-xpon-config/description
  Disabled-by-default UCI configuration and manual lab loader for econet-xpon.
  It never creates a persistent modules.d entry, always keeps tx_laser_off=1,
  extracts and validates the per-unit EN7570 matrix from read-only factory MTD,
  and does not log subscriber identity or credentials.
endef

define Build/Compile
endef

define Package/econet-xpon-config/conffiles
/etc/config/econet-xpon
endef

define Package/econet-xpon-config/install
	$(INSTALL_DIR) $(1)/etc/config $(1)/etc/init.d
	$(INSTALL_CONF) ./files/etc/config/econet-xpon $(1)/etc/config/econet-xpon
	$(INSTALL_BIN) ./files/etc/init.d/econet-xpon $(1)/etc/init.d/econet-xpon
endef

$(eval $(call BuildPackage,econet-xpon-config))

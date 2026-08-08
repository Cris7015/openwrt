include $(TOPDIR)/rules.mk

PKG_NAME:=econet-omcid
PKG_VERSION:=1
PKG_RELEASE:=1
PKG_MAINTAINER:=AK Sharma <maskachoska@yahoo.com>
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=COPYING

include $(INCLUDE_DIR)/package.mk

define Package/econet-omcid
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=WWAN
  TITLE:=EcoNet EN75xx experimental H660-style OMCI lab daemon
  DEPENDS:=@TARGET_econet +kmod-econet-xpon
endef

define Package/econet-omcid/description
  Experimental clean-room baseline OMCI prototype imported from PR 24577. Its
  pre-instantiated MIB models an H660-style HGU and is not an XR500v profile.
  It has no init script and requires an explicit runtime opt-in. No subscriber
  identity is compiled into the binary.
endef

# Single self-contained C source (libc + linux headers only).
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -Wall -Wextra -Wformat=2 \
		$(TARGET_LDFLAGS) \
		-o $(PKG_BUILD_DIR)/omcid $(PKG_BUILD_DIR)/omcid.c
endef

define Package/econet-omcid/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omcid $(1)/usr/sbin/omcid
endef

$(eval $(call BuildPackage,econet-omcid))

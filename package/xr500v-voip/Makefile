#
# XR500v FXS VoIP userspace: the baresip "xr500v" audio driver module
# (SLIC over /dev/xr500v-voice), the xr500v-callmgr (SLIC hook <-> baresip
# ctrl_tcp), a baresip config and a procd service that brings it all up.
#
include $(TOPDIR)/rules.mk

PKG_NAME:=xr500v-voip
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=Cris7015 <pcristian292@gmail.com>

include $(INCLUDE_DIR)/package.mk

define Package/xr500v-voip
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Telephony
  TITLE:=TP-Link Archer XR500v FXS telephony (baresip + call-manager)
  DEPENDS:=@TARGET_econet +kmod-econet-pcm +baresip +baresip-mod-g711 \
	   +baresip-mod-l16 +baresip-mod-ctrl_tcp
endef

define Package/xr500v-voip/description
  Turnkey FXS telephony for the Archer XR500v: the baresip "xr500v" audio
  driver module (reads/writes the Le9642 SLIC line through the kernel char
  device /dev/xr500v-voice), the xr500v-callmgr that bridges the SLIC hook to
  baresip's ctrl_tcp control interface (ring / answer / hang-up), a baresip
  config, and a procd service that loads the SLIC channel, starts baresip and
  the call-manager at boot.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

# The call-manager is plain musl C (sockets + debugfs, no external libs).
# The baresip module links the staged libre/libbaresip and is dlopen'd by
# baresip at runtime.
define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(PKG_BUILD_DIR)/xr500v-callmgr $(PKG_BUILD_DIR)/xr500v-callmgr.c
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -fPIC -shared \
		-D_DEFAULT_SOURCE \
		-I$(STAGING_DIR)/usr/include -I$(STAGING_DIR)/usr/include/re \
		-o $(PKG_BUILD_DIR)/xr500v.so $(PKG_BUILD_DIR)/xr500v.c \
		-L$(STAGING_DIR)/usr/lib -lbaresip -lre -lpthread
endef

define Package/xr500v-voip/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/lib/baresip/modules \
		$(1)/etc/xr500v-voip $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/xr500v-callmgr $(1)/usr/sbin/xr500v-callmgr
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/xr500v.so $(1)/usr/lib/baresip/modules/xr500v.so
	$(INSTALL_CONF) ./files/config $(1)/etc/xr500v-voip/config
	$(INSTALL_DATA) ./files/accounts.template $(1)/etc/xr500v-voip/accounts.template
	$(INSTALL_DATA) ./files/contacts $(1)/etc/xr500v-voip/contacts
	$(INSTALL_BIN) ./files/voip.init $(1)/etc/init.d/voip
endef

$(eval $(call BuildPackage,xr500v-voip))

#
# XR500v FXS VoIP userspace: the baresip "xr500v" audio driver module
# (SLIC over /dev/xr500v-voice), the xr500v-callmgr (SLIC hook <-> baresip
# ctrl_tcp), a baresip config and a procd service that brings it all up.
#
include $(TOPDIR)/rules.mk

PKG_NAME:=xr500v-voip
PKG_RELEASE:=11
PKG_BUILD_DEPENDS:=speexdsp
PKG_LICENSE:=GPL-2.0-or-later BSD-3-Clause
PKG_MAINTAINER:=Cristian Papa <pcristian292@gmail.com>

include $(INCLUDE_DIR)/package.mk

define Package/xr500v-voip
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Telephony
  TITLE:=TP-Link Archer XR500v FXS telephony (baresip + call-manager)
  DEPENDS:=@TARGET_airoha_en751221 +kmod-econet-pcm +baresip +baresip-mod-g711 \
	   +baresip-mod-l16 +baresip-mod-ctrl_tcp +libjson-c +ucode +ucode-mod-fs +ucode-mod-uci +ucode-mod-ubus
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
		-o $(PKG_BUILD_DIR)/xr500v-callmgr $(PKG_BUILD_DIR)/xr500v-callmgr.c -lm -ljson-c
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -fPIC -shared \
		-D_DEFAULT_SOURCE \
		-I$(STAGING_DIR)/usr/include -I$(STAGING_DIR)/usr/include/re \
		-o $(PKG_BUILD_DIR)/xr500v.so $(PKG_BUILD_DIR)/xr500v.c \
		-L$(STAGING_DIR)/usr/lib -lbaresip -lre -lpthread \
		$(STAGING_DIR)/usr/lib/libspeexdsp.a -lm
endef

define Package/xr500v-voip/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/lib/baresip/modules \
		$(1)/etc/xr500v-voip/phone1 $(1)/etc/xr500v-voip/phone2 \
		$(1)/etc/init.d $(1)/etc/config $(1)/usr/libexec $(1)/lib/upgrade/keep.d \
		$(1)/usr/share/xr500v-voip
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/xr500v-callmgr $(1)/usr/sbin/xr500v-callmgr
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/xr500v.so $(1)/usr/lib/baresip/modules/xr500v.so
	$(INSTALL_CONF) ./files/config $(1)/etc/xr500v-voip/config
	$(INSTALL_DATA) ./files/accounts.template $(1)/etc/xr500v-voip/accounts.template
	$(INSTALL_DATA) ./files/contacts $(1)/etc/xr500v-voip/contacts
	$(INSTALL_BIN) ./files/voip.init $(1)/etc/init.d/voip
	$(INSTALL_BIN) ./files/voip-render.sh $(1)/usr/libexec/xr500v-voip-render
	$(INSTALL_BIN) ./files/voip-profile.uc $(1)/usr/libexec/xr500v-voip-profile
	$(INSTALL_DATA) ./files/accounts.uc $(1)/usr/share/xr500v-voip/accounts.uc
	$(INSTALL_CONF) ./files/voip.config $(1)/etc/config/xr500v-voip
	$(INSTALL_CONF) ./files/phone1.accounts.template $(1)/etc/xr500v-voip/phone1/accounts.template
	$(INSTALL_CONF) ./files/phone2.accounts.template $(1)/etc/xr500v-voip/phone2/accounts.template
	$(INSTALL_DATA) ./files/sysupgrade.keep $(1)/lib/upgrade/keep.d/xr500v-voip
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/uci-defaults-log-size $(1)/etc/uci-defaults/95-xr500v-voip-log-size
	$(INSTALL_DATA) ./files/README ./files/speexdsp.COPYING $(1)/usr/share/xr500v-voip/
endef

define Package/xr500v-voip/conffiles
/etc/xr500v-voip/config
/etc/xr500v-voip/accounts.template
/etc/xr500v-voip/contacts
/etc/config/xr500v-voip
/etc/xr500v-voip/phone1/accounts.template
/etc/xr500v-voip/phone2/accounts.template
endef

$(eval $(call BuildPackage,xr500v-voip))

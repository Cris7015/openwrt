# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
LUCI_TITLE:=XR500v GPON fiber health
LUCI_PKGARCH:=all
LUCI_DEPENDS:=+luci-base +ucode +ucode-mod-fs +ucode-mod-ubus +ucode-mod-uci +ucode-mod-uloop +omci-agent

define Package/luci-app-xr500v-gpon/conffiles
/etc/config/xr500v-gpon
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature

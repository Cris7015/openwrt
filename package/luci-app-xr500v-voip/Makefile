# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_VERSION:=0.4.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
LUCI_TITLE:=XR500v dual-FXS telephony status and accounts
LUCI_PKGARCH:=all
LUCI_DEPENDS:=+luci-base +rpcd-mod-ucode +ucode-mod-fs +ucode-mod-ubus +ucode-mod-uci +xr500v-voip

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature

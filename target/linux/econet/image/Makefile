include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

define Target/Description
	Build firmware images for EcoNet MIPS based boards.
endef

# tclinux-trx is the default format used in the SDK
define Build/tclinux-trx
  ./tclinux-trx.sh --kernel $@ --rootfs $(IMAGE_ROOTFS) \
    --version $(VERSION_DIST)-$(REVISION) --endian $(TRX_ENDIAN) \
    $(if $(TRX_MODEL),--model $(TRX_MODEL)) > $@.new
	mv $@.new $@
endef

# Build TRX without rootfs, for use with UBI root
define Build/kernel-trx
  ./tclinux-trx.sh --kernel $@ \
    --version $(VERSION_DIST)-$(REVISION) --endian $(TRX_ENDIAN) \
    $(if $(TRX_MODEL),--model $(TRX_MODEL)) > $@.new
	mv $@.new $@
endef

# Build and validate the non-standard 512-byte TrendChip wrapper consumed by
# the TP-Link Archer XR500v BLDR.
define Build/xr500v-trendchip-header
	python3 ./xr500v-trendchip-header.py $@
endef

# If TCSUPPORT_FREE_BOOTBASE is enabled, bootloader will decompress the kernel
# at 0x80002000 instead of 0x80020000.
#
# You can tell it by looking at the bootloader version string
# " at %s version %s free bootbase".
define Build/tclinux-free-bootbase-jump
	@[ "$(KERNEL_LOADADDR)" = 0x80020000 ] || { \
		$(call ERROR_MESSAGE,WARNING: KERNEL_LOADADDR changed); \
		rm -f $@; \
	}
	printf '\010\000\200\000' > $@.new
	dd if=/dev/zero bs=122876 count=1 >> $@.new
	cat $@ >> $@.new
	mv $@.new $@
endef

# tclinux bootloader requires LZMA, BUT only provides 7.5MB of space
# to decompress into. So we use vmlinuz and decompress twice.
define Device/Default
  DEVICE_DTS_DIR := ../dts
  BLOCKSIZE := 128k
  PAGESIZE := 2048
  # decompression buffer limit, the size of LZMA-compressed data is read from
  # the trx header
  KERNEL_SIZE := 7480k
  KERNEL_NAME := vmlinuz.bin
  KERNEL_LOADADDR := 0x80020000
  KERNEL := kernel-bin | append-dtb
  UBINIZE_OPTS := -E 5
endef

include $(SUBTARGET).mk

$(eval $(call BuildImage))

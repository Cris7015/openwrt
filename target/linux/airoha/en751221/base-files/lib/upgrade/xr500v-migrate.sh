# TP-Link Archer XR500v v1: move from the OEM bootloader (tcboot) layout to
# OpenWrt's U-Boot + UBI layout without a serial console.
#
# xr500v-migrate-uboot checks everything while the system is up and stages in
# RAM: U-Boot (tcboot.bin), the U-Boot sysupgrade image, the OEM "misc"
# partition (MAC address, EN7570 calibration, MT7662 EEPROM), the OEM
# bootloader and the first MiB of the flash as it is now. Then it runs
# sysupgrade on the U-Boot image, and platform.sh hands that image to
# xr500v_migrate_stage2, which refuses anything that was not staged.
#
# The order makes every failure land on something that boots:
#  1. U-Boot goes into the first MiB and is read back. If it cannot be
#     verified, the original first MiB is written back and nothing else is
#     touched: the OEM bootloader and OpenWrt still boot.
#  2. From here on U-Boot boots whatever happens next; with no OpenWrt to
#     load, it falls back to TFTP recovery. The rest of the flash becomes one
#     UBI device: first the volumes U-Boot's format_ubi_part creates (misc and
#     tcboot_oem with the OEM data, ubootenv and ubootenv2 empty), then "fit"
#     and "rootfs_data" as a U-Boot sysupgrade leaves them.
#  3. Everything is read back and the configuration goes into rootfs_data.
#
# The flash is written through the kernel's view of the OEM bad block table.
# UBI does not depend on where its blocks are, so blocks the table moved are
# found by U-Boot anyway; only the first MiB has to be where the boot ROM
# reads it, which is checked before anything is staged.

XR500V_MIGRATE_DIR=/tmp/xr500v-migrate

# Fixed sizes of the OEM data and of the U-Boot layout.
XR500V_MIGRATE_UBOOT_SIZE=1048576
XR500V_MIGRATE_MISC_SIZE=4718592
XR500V_MIGRATE_OEMBOOT_SIZE=262144
XR500V_MIGRATE_ENV_SIZE=126976

xr500v_migrate_msg() {
	echo "xr500v-migrate: $*" >&2
}

xr500v_migrate_hex() {
	dd if="$1" bs=1 skip="$2" count="$3" 2>/dev/null |
		hexdump -v -e '1/1 "%02x"'
}

xr500v_migrate_size() {
	wc -c < "$1" 2>/dev/null
}

xr500v_migrate_md5() {
	local sum

	sum=$(md5sum "$1" 2>/dev/null) || return 1
	echo "${sum%% *}"
}

# U-Boot for this board, as the release ships it: the 1 MiB first-stage image
# (DDR stage, flash loader and U-Boot) with this board's device tree.
xr500v_migrate_check_uboot() {
	local file="$1"

	[ "$(xr500v_migrate_size "$file")" = "$XR500V_MIGRATE_UBOOT_SIZE" ] || {
		xr500v_migrate_msg "$file is not 1 MiB: not the tcboot.bin of a release"
		return 1
	}
	[ "$(xr500v_migrate_hex "$file" 12 4)" = "36353738" ] || {
		xr500v_migrate_msg "$file has no EN751221 boot header"
		return 1
	}
	grep -qF "U-Boot 20" "$file" && grep -qF "tplink,archer-xr500v-v1" "$file" || {
		xr500v_migrate_msg "$file is not U-Boot for the Archer XR500v"
		return 1
	}
}

# The U-Boot sysupgrade image: a FIT whose metadata names this board.
xr500v_migrate_check_fit() {
	local file="$1"
	local size total

	[ "$(xr500v_migrate_hex "$file" 0 4)" = "d00dfeed" ] || {
		xr500v_migrate_msg "$file is not the U-Boot sysupgrade image (no FIT header)"
		return 1
	}
	# The structure only: kernel and rootfs follow it (external data).
	size=$(xr500v_migrate_size "$file")
	total=$((0x$(xr500v_migrate_hex "$file" 4 4)))
	if [ "$size" -lt 1048576 ] || [ "$size" -gt 33554432 ] ||
	   [ "$total" -gt "$size" ]; then
		xr500v_migrate_msg "$file is truncated or too large"
		return 1
	fi
	fwtool -q -i - "$file" 2>/dev/null |
		grep -qF '"tplink,archer-xr500v-v1"' || {
		xr500v_migrate_msg "$file has no OpenWrt metadata for the Archer XR500v v1"
		return 1
	}
	if command -v fit_check_sign >/dev/null; then
		fit_check_sign -f "$file" >/dev/null 2>&1 || {
			xr500v_migrate_msg "$file fails its own FIT hashes"
			return 1
		}
	fi
}

# The OEM "misc" partition, checked like U-Boot's format_ubi_part checks it:
# MAC address at 0xf100, EN7570 calibration magic at 0x20094, MT7662 EEPROM
# at 0xe0000.
xr500v_migrate_check_misc() {
	local file="$1"
	local mac

	[ "$(xr500v_migrate_size "$file")" = "$XR500V_MIGRATE_MISC_SIZE" ] || return 1
	mac=$(xr500v_migrate_hex "$file" $((0xf100)) 6)
	case "$mac" in
	ffffffffffff|000000000000|"") return 1 ;;
	esac
	[ "$(xr500v_migrate_hex "$file" $((0x20094)) 4)" = "07050700" ] &&
		[ "$(xr500v_migrate_hex "$file" $((0xe0000)) 2)" = "6276" ]
}

# The OEM bootloader: the EN751221 boot header and the TrendChip notice of
# the vendor's boot code, which OpenWrt's tcboot.bin does not have.
xr500v_migrate_check_oemboot() {
	local file="$1"

	[ "$(xr500v_migrate_size "$file")" = "$XR500V_MIGRATE_OEMBOOT_SIZE" ] &&
		[ "$(xr500v_migrate_hex "$file" 12 4)" = "36353738" ] &&
		grep -qF "TrendChip Technologies Corp." "$file" &&
		! grep -qF "U-Boot 20" "$file"
}

# The boot ROM reads U-Boot from the first eight physical blocks. The kernel
# writes through the OEM bad block table, so a factory bad or remapped block
# among them would put part of U-Boot somewhere else. The table is only
# printed at boot.
xr500v_migrate_check_bmt() {
	local log

	log=$( { cat /tmp/xr500v-bmt.log; dmesg; } 2>/dev/null)
	echo "$log" | grep -q "en75_bmt: blocks: total:" || {
		xr500v_migrate_msg "the bad block table summary is no longer in the kernel log: reboot and run this again"
		return 1
	}
	echo "$log" | awk '
		/ - BBT factory bad block: / { if ($NF + 0 < 8) bad = 1 }
		/ - BMT mapped worn block: / { split($NF, b, "->"); if (b[1] + 0 < 8) bad = 1 }
		END { exit bad }' || {
		xr500v_migrate_msg "a bad or remapped block lies in the first MiB: this router needs the serial console method"
		return 1
	}
}

xr500v_migrate_check_layout() {
	local part index

	for part in bootloader misc openwrt_ubi migrate_uboot migrate_ubi; do
		[ -n "$(find_mtd_index "$part")" ] || {
			xr500v_migrate_msg "partition $part is missing: this is not the OEM bootloader image of this release"
			return 1
		}
	done
	[ -z "$(find_mtd_index u-boot)" ] || {
		xr500v_migrate_msg "this router already runs U-Boot"
		return 1
	}
	index=$(find_mtd_index migrate_uboot)
	[ "$(cat "/sys/class/mtd/mtd$index/size")" = "$XR500V_MIGRATE_UBOOT_SIZE" ] || {
		xr500v_migrate_msg "migrate_uboot is not 1 MiB"
		return 1
	}
}

# The image sysupgrade got is the one staged by xr500v-migrate-uboot.
xr500v_migrate_armed() {
	local image="$1"
	local armed

	[ -f "$XR500V_MIGRATE_DIR/armed" ] || return 1
	armed=$(cat "$XR500V_MIGRATE_DIR/armed")
	[ -n "$armed" ] && [ "$(xr500v_migrate_md5 "$image")" = "$armed" ]
}

# Read back exactly the size of the file; head loops over short reads of the
# MTD and UBI character devices.
xr500v_migrate_readback() {
	local file="$1"
	local device="$2"
	local sum

	sum=$(head -c "$(xr500v_migrate_size "$file")" "$device" | md5sum)
	[ "${sum%% *}" = "$(xr500v_migrate_md5 "$file")" ]
}

xr500v_migrate_write_boot() {
	local file="$1"
	local index

	index=$(find_mtd_index migrate_uboot)
	mtd -f write "$file" migrate_uboot >/dev/console 2>&1 &&
		xr500v_migrate_readback "$file" "/dev/mtd$index"
}

xr500v_migrate_volume() {
	local name="$1"
	local id="$2"
	local size="$3"
	local type="$4"
	local file="$5"
	local dm extent="-m"

	# No size: all the space left, as sysupgrade creates rootfs_data.
	[ -n "$size" ] && extent="-s $size"
	# shellcheck disable=SC2086
	ubimkvol /dev/ubi0 -n "$id" -N "$name" $extent \
		${type:+-t "$type"} >/dev/console 2>&1 || {
		xr500v_migrate_msg "cannot create the $name volume"
		return 1
	}
	if [ ! -e "/dev/ubi0_$id" ]; then
		dm=$(cat "/sys/class/ubi/ubi0_$id/dev") &&
			mknod "/dev/ubi0_$id" c "${dm%:*}" "${dm#*:}"
	fi
	[ -n "$file" ] || return 0
	ubiupdatevol "/dev/ubi0_$id" "$file" >/dev/console 2>&1 &&
		xr500v_migrate_readback "$file" "/dev/ubi0_$id" || {
		xr500v_migrate_msg "cannot write or verify the $name volume"
		return 1
	}
}

xr500v_migrate_ubi() {
	local image="$1"
	local index dm

	index=$(find_mtd_index migrate_ubi)
	ubiformat "/dev/mtd$index" -y >/dev/console 2>&1 || return 1
	ubiattach -m "$index" -d 0 >/dev/console 2>&1 || return 1
	if [ ! -e /dev/ubi0 ]; then
		dm=$(cat /sys/class/ubi/ubi0/dev) &&
			mknod /dev/ubi0 c "${dm%:*}" "${dm#*:}" || return 1
	fi

	# The OEM data first, with the IDs format_ubi_part gives them.
	xr500v_migrate_volume misc 2 "$XR500V_MIGRATE_MISC_SIZE" static \
		"$XR500V_MIGRATE_DIR/misc.bin" &&
	xr500v_migrate_volume tcboot_oem 3 "$XR500V_MIGRATE_OEMBOOT_SIZE" static \
		"$XR500V_MIGRATE_DIR/oem-bootloader.bin" &&
	xr500v_migrate_volume ubootenv 0 "$XR500V_MIGRATE_ENV_SIZE" static &&
	xr500v_migrate_volume ubootenv2 1 "$XR500V_MIGRATE_ENV_SIZE" static &&
	xr500v_migrate_volume fit 4 "$(xr500v_migrate_size "$image")" "" "$image" &&
	xr500v_migrate_volume rootfs_data 5 "" "" || return 1

	if [ -n "$UPGRADE_BACKUP" ] && [ -f "$UPGRADE_BACKUP" ]; then
		mkdir -p /tmp/new_root
		mount -t ubifs ubi0:rootfs_data /tmp/new_root >/dev/console 2>&1 &&
			cp "$UPGRADE_BACKUP" /tmp/new_root/sysupgrade.tgz &&
			sync && umount /tmp/new_root || {
			xr500v_migrate_msg "could not keep the configuration"
			umount /tmp/new_root 2>/dev/null
		}
	fi
	ubidetach -d 0 >/dev/console 2>&1
	sync
}

xr500v_migrate_stage2() {
	local image="$1"
	local d="$XR500V_MIGRATE_DIR"

	xr500v_migrate_armed "$image" ||
		xr500v_upgrade_fail "this U-Boot image was not staged by xr500v-migrate-uboot"
	(cd "$d" && md5sum -c manifest >/dev/null 2>&1) ||
		xr500v_upgrade_fail "the staged files changed since they were checked"

	# Release the OpenWrt overlay before the flash under it goes away.
	if [ -d /sys/class/ubi/ubi0 ]; then
		ubidetach -d 0 >/dev/console 2>&1 ||
			xr500v_upgrade_fail "cannot detach openwrt_ubi"
	fi

	xr500v_migrate_msg "writing U-Boot to the first MiB"
	if ! xr500v_migrate_write_boot "$d/u-boot.bin" &&
	   ! xr500v_migrate_write_boot "$d/u-boot.bin"; then
		xr500v_migrate_msg "U-Boot could not be verified: putting the OEM bootloader back"
		xr500v_migrate_write_boot "$d/first-mib.bin" ||
			xr500v_migrate_write_boot "$d/first-mib.bin" ||
			xr500v_migrate_msg "the first MiB could not be restored either: the serial console is needed now"
		xr500v_upgrade_fail "U-Boot was not installed; the OEM layout is unchanged"
	fi

	xr500v_migrate_msg "U-Boot installed; creating the UBI volumes"
	if ! xr500v_migrate_ubi "$image"; then
		xr500v_migrate_msg "the UBI setup failed; U-Boot will start its TFTP recovery"
		sync
		return 1
	fi
	rm -f "$d/armed"
	xr500v_migrate_msg "done: U-Boot boots OpenWrt from UBI after this reboot"
}

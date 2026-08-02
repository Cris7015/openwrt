RAMFS_COPY_BIN='tr'

case "$(board_name)" in
tplink,archer-xr500v-v1)
	REQUIRE_IMAGE_METADATA=1
	;;
esac

xr500v_upgrade_fail() {
	rm -f /tmp/xr500v-kernel1.bin /tmp/xr500v-rootfs1.bin
	echo "XR500v upgrade aborted: $*" >&2
	exit 1
}

xr500v_image_hex() {
	local image="$1"
	local offset="$2"
	local count="$3"

	dd if="$image" bs=1 skip="$offset" count="$count" 2>/dev/null |
		hexdump -v -e '1/1 "%02x"'
}

xr500v_write_verify() {
	local image="$1"
	local partition="$2"
	local index device size blocks image_md5 flash_md5

	mtd -f write "$image" "$partition" ||
		xr500v_upgrade_fail "write failed for $partition"

	index=$(find_mtd_index "$partition")
	[ -n "$index" ] ||
		xr500v_upgrade_fail "cannot resolve $partition after writing"
	device="/dev/mtd$index"
	size=$(wc -c < "$image")
	[ $((size % 131072)) -eq 0 ] ||
		xr500v_upgrade_fail "$partition image is not eraseblock aligned"
	blocks=$((size / 131072))
	image_md5=$(md5sum "$image")
	image_md5=${image_md5%% *}
	flash_md5=$(dd if="$device" bs=131072 count="$blocks" 2>/dev/null |
		md5sum)
	flash_md5=${flash_md5%% *}
	if [ -z "$image_md5" ] || [ "$image_md5" != "$flash_md5" ]; then
		xr500v_upgrade_fail "readback verification failed for $partition"
	fi
}

xr500v_ubi_make_devnode() {
	local devname="$1"
	local dm

	[ -e "/dev/$devname" ] && return 0
	[ -r "/sys/class/ubi/$devname/dev" ] || return 1
	dm=$(cat "/sys/class/ubi/$devname/dev")
	mknod "/dev/$devname" c "${dm%:*}" "${dm#*:}"
}

xr500v_ubi_rootfs_data() {
	local candidate

	for candidate in /sys/class/ubi/ubi0_*; do
		[ -r "$candidate/name" ] || continue
		[ "$(cat "$candidate/name")" = "rootfs_data" ] || continue
		echo "${candidate##*/}"
		return 0
	done

	return 1
}

xr500v_prepare_ubi_overlay() {
	local mtdnum mounted_mtd volume

	mtdnum=$(find_mtd_index openwrt_ubi)
	[ -n "$mtdnum" ] ||
		xr500v_upgrade_fail "openwrt_ubi is missing; install this release from its initramfs image"
	[ "$(cat "/sys/class/mtd/mtd$mtdnum/size" 2>/dev/null)" = "67108864" ] ||
		xr500v_upgrade_fail "openwrt_ubi is not exactly 64 MiB"

	if [ -d /sys/class/ubi/ubi0 ]; then
		mounted_mtd=$(cat /sys/class/ubi/ubi0/mtd_num 2>/dev/null)
		[ "$mounted_mtd" = "$mtdnum" ] ||
			xr500v_upgrade_fail "ubi0 belongs to mtd$mounted_mtd instead of openwrt_ubi"
	fi

	if [ -n "$UPGRADE_BACKUP" ]; then
		if [ ! -d /sys/class/ubi/ubi0 ]; then
			ubiattach -m "$mtdnum" -d 0 >/dev/console 2>&1 ||
				xr500v_upgrade_fail "cannot attach openwrt_ubi while preserving configuration; retry with -n only if erasing it is intended"
		fi
		volume=$(xr500v_ubi_rootfs_data) ||
			xr500v_upgrade_fail "rootfs_data is missing while configuration preservation was requested"
		echo "Preserving the XR500v rootfs_data UBI volume" >&2
		return 0
	fi

	# -n has an explicit no-preserve meaning.  Recreate only the dedicated
	# OpenWrt partition; the OEM slot, active OpenWrt slot and BMT reserve are
	# separate MTD regions and are never passed to these tools.
	if [ -d /sys/class/ubi/ubi0 ]; then
		ubidetach /dev/ubi_ctrl -d 0 >/dev/console 2>&1 ||
			xr500v_upgrade_fail "cannot detach openwrt_ubi before reprovisioning"
	fi
	ubiformat "/dev/mtd$mtdnum" -y >/dev/console 2>&1 ||
		xr500v_upgrade_fail "could not format openwrt_ubi"
	ubiattach -m "$mtdnum" -d 0 >/dev/console 2>&1 ||
		xr500v_upgrade_fail "could not attach the freshly formatted openwrt_ubi"
	xr500v_ubi_make_devnode ubi0 ||
		xr500v_upgrade_fail "cannot create the ubi0 device node"
	ubimkvol /dev/ubi0 -N rootfs_data -m >/dev/console 2>&1 ||
		xr500v_upgrade_fail "could not create rootfs_data"
	volume=$(xr500v_ubi_rootfs_data) ||
		xr500v_upgrade_fail "rootfs_data was not visible after creation"
	xr500v_ubi_make_devnode "$volume" ||
		xr500v_upgrade_fail "cannot create the $volume device node"

	mkdir -p /tmp/.xr500v-ubifs-provision
	mount -t ubifs ubi0:rootfs_data /tmp/.xr500v-ubifs-provision \
		>/dev/console 2>&1 ||
		xr500v_upgrade_fail "could not initialize rootfs_data as UBIFS"
	sync
	umount /tmp/.xr500v-ubifs-provision >/dev/console 2>&1 ||
		xr500v_upgrade_fail "could not unmount the initialized rootfs_data"
	rmdir /tmp/.xr500v-ubifs-provision
	echo "Reprovisioned the XR500v rootfs_data UBI volume (-n)" >&2
}

platform_check_image() {
	local board
	local image="$1"
	local base_image base_size rootfs_payload payload_hex rootfs_hex
	local gap_nonzero squashfs_hex

	board=$(board_name)

	case "$board" in
	chinamobile,gs3101)
		return 0
		;;
	tplink,archer-xr500v-v1)
		base_image=$(mktemp /tmp/xr500v-image.XXXXXX) || return 1
		if ! fwtool -q -T -i /dev/null "$image" > "$base_image"; then
			rm -f "$base_image"
			echo "Invalid image: OpenWrt metadata trailer is missing or corrupt"
			return 1
		fi

		base_size=$(wc -c < "$base_image") || {
			rm -f "$base_image"
			return 1
		}
		rootfs_payload=$((base_size - 0x300200))
		if [ "$rootfs_payload" -lt 96 ] ||
		   [ "$rootfs_payload" -gt $((0x1000000)) ]; then
			rm -f "$base_image"
			echo "Invalid image: rootfs payload does not fit rootfs1"
			return 1
		fi

		if [ "$(dd if="$base_image" bs=1 skip=$((0x300200)) count=4 \
			2>/dev/null | hexdump -v -e '1/1 "%02x"')" != "68737173" ]; then
			rm -f "$base_image"
			echo "Invalid image: no squashfs at 0x300200"
			return 1
		fi

		payload_hex=$(printf '%08x' $((base_size - 0x200)))
		rootfs_hex=$(printf '%02x%02x%02x%02x00000000' \
			$((rootfs_payload & 0xff)) \
			$(((rootfs_payload >> 8) & 0xff)) \
			$(((rootfs_payload >> 16) & 0xff)) \
			$(((rootfs_payload >> 24) & 0xff)))
		squashfs_hex=$(xr500v_image_hex "$base_image" \
			$((0x300200 + 40)) 8)
		if [ "$squashfs_hex" != "$rootfs_hex" ]; then
			rm -f "$base_image"
			echo "Invalid image: SquashFS bytes_used does not match payload"
			return 1
		fi

		gap_nonzero=$(dd if="$base_image" bs=1 skip=$((0x300000)) \
			count=$((0x200)) 2>/dev/null | tr -d '\000' | wc -c)
		if [ "$gap_nonzero" -ne 0 ]; then
			rm -f "$base_image"
			echo "Invalid image: gap at 0x300000 is not all zero"
			return 1
		fi

		if [ "$(xr500v_image_hex "$base_image" $((0x00)) 13)" != \
		     "030000007665722e20322e3000" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x34)) 8)" != \
		     "0ec6000100000001" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x60)) 8)" != \
		     "4c3d2e1faa55aa55" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x68)) 4)" != "80020000" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x6c)) 4)" != "80020000" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x70)) 4)" != "01300000" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x74)) 4)" != "00000200" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x78)) 4)" != "$payload_hex" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x7c)) 4)" != "00300000" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x80)) 4)" != "01000000" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x88)) 4)" != "00000000" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x8c)) 8)" != \
		     "55aa0101a5000000" ]; then
			rm -f "$base_image"
			echo "Invalid image: incomplete XR500v TrendChip header"
			return 1
		fi

		rm -f "$base_image"
		return 0
		;;
	esac

	return 1
}

platform_do_upgrade() {
	local board
	local kernel_image=/tmp/xr500v-kernel1.bin
	local rootfs_image=/tmp/xr500v-rootfs1.bin

	board=$(board_name)

	case "$board" in
	chinamobile,gs3101)
		CI_KERNPART="tclinux_kernel"
		nand_do_upgrade "$1"
		;;
	tplink,archer-xr500v-v1)
		# fwtool metadata is for sysupgrade validation only.  Strip it before
		# slicing so the hardware receives the exact BLDR-validated container.
		fwtool -q -t -i /dev/null "$1" ||
			xr500v_upgrade_fail "could not strip the metadata trailer"

		rm -f "$kernel_image" "$rootfs_image"
		dd if="$1" of="$kernel_image" bs=131072 count=24 2>/dev/null ||
			xr500v_upgrade_fail "could not extract kernel1"
		[ "$(wc -c < "$kernel_image")" -eq $((0x300000)) ] ||
			xr500v_upgrade_fail "kernel1 slice is not exactly 3 MiB"

		# Fill all of rootfs1 with 0xff before overlaying the squashfs payload.
		# This erases stale JFFS2/config data from earlier installations.
		dd if=/dev/zero bs=131072 count=128 2>/dev/null |
			tr '\000' '\377' > "$rootfs_image" ||
			xr500v_upgrade_fail "could not allocate the rootfs1 image"
		dd if="$1" of="$rootfs_image" bs=512 skip=6145 \
			conv=notrunc 2>/dev/null ||
			xr500v_upgrade_fail "could not extract rootfs1"
		[ "$(wc -c < "$rootfs_image")" -eq $((0x1000000)) ] ||
			xr500v_upgrade_fail "rootfs1 slice is not exactly 16 MiB"

		xr500v_prepare_ubi_overlay

		# Rootfs first, then the boot-critical kernel.  Every write is read back
		# before continuing; xr500v_upgrade_fail exits stage2 on any mismatch.
		xr500v_write_verify "$rootfs_image" rootfs1
		xr500v_write_verify "$kernel_image" kernel1

		sync
		rm -f "$kernel_image" "$rootfs_image"
		return 0
		;;
	esac
}
